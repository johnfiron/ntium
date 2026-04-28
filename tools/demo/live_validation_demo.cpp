#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/ingest/SnapshotParser.h"
#include "src/ingest/SnapshotSchema.h"
#include "src/input/ArbitrationEngine.h"
#include "src/input/InputRouter.h"
#include "src/ipc/PipeProtocol.h"
#include "src/overlay/DirtyRegionGenerator.h"
#include "src/overlay/EventStore.h"
#include "src/overlay/OverlayThrottle.h"
#include "src/render/DirtyRectCoalescer.h"
#include "src/scene/CameraController.h"
#include "src/scene/GeoMath.h"

namespace {

using Clock = std::chrono::steady_clock;

struct CheckResult {
  std::string name;
  bool pass = false;
  std::string detail;
};

void WriteF32Le(std::uint8_t* out, std::size_t offset, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  ntium::ipc::WriteU32Le(out, offset, bits);
}

std::vector<std::uint8_t> BuildValidSnapshot() {
  using namespace ntium::ingest;

  const PointAlertPayloadPrefixWire payload{
      .x_px = 120,
      .y_px = 240,
      .size_px = 12,
      .style_id = 1,
      .pulse_period_ms = 1000,
      .severity = 2,
      .z_index = 3,
      .color_argb = 0xFF00FF00u,
  };

  RecordEnvelopeWire envelope{};
  envelope.record_size = static_cast<std::uint16_t>(
      sizeof(RecordEnvelopeWire) + sizeof(PointAlertPayloadPrefixWire));
  envelope.record_type = static_cast<std::uint8_t>(RecordType::kPointAlert);
  envelope.record_flags = 0;
  envelope.schema_version = 1;
  envelope.reserved0 = 0;
  envelope.reserved1 = 0;
  envelope.event_id = 42;
  envelope.created_unix_ms = 1'700'000'000'000ull;
  envelope.ttl_ms = 2000;
  envelope.payload_size = sizeof(PointAlertPayloadPrefixWire);
  envelope.reserved2 = 0;
  envelope.payload_crc32c = ComputeCrc32c(
      reinterpret_cast<const std::uint8_t*>(&payload), sizeof(payload));

  SnapshotHeaderWire header{};
  header.magic = kSnapshotMagic;
  header.format_version = kSnapshotFormatVersion;
  header.header_size = kSnapshotHeaderSize;
  header.snapshot_kind = static_cast<std::uint8_t>(SnapshotKind::kFullState);
  header.header_flags = 0;
  header.reserved0 = 0;
  header.sequence = 1;
  header.base_sequence = 0;
  header.generated_unix_ms = envelope.created_unix_ms;
  header.canvas_width_px = 1920;
  header.canvas_height_px = 1080;
  header.record_count = 1;
  header.records_bytes = envelope.record_size;
  header.header_crc32c = 0;
  header.snapshot_crc32c = 0;
  header.reserved1 = 0;

  std::vector<std::uint8_t> snapshot(
      kSnapshotHeaderSize + envelope.record_size, 0);
  std::memcpy(snapshot.data(), &header, sizeof(header));
  std::memcpy(snapshot.data() + kSnapshotHeaderSize, &envelope, sizeof(envelope));
  std::memcpy(snapshot.data() + kSnapshotHeaderSize + sizeof(envelope),
              &payload, sizeof(payload));

  std::vector<std::uint8_t> header_copy(snapshot.begin(),
                                        snapshot.begin() + kSnapshotHeaderSize);
  for (std::size_t i = 48; i <= 55; ++i) {
    header_copy[i] = 0;
  }
  header.header_crc32c = ComputeCrc32c(header_copy.data(), header_copy.size());
  std::memcpy(snapshot.data(), &header, sizeof(header));

  std::vector<std::uint8_t> snapshot_copy = snapshot;
  for (std::size_t i = 52; i <= 55; ++i) {
    snapshot_copy[i] = 0;
  }
  header.snapshot_crc32c =
      ComputeCrc32c(snapshot_copy.data(), snapshot_copy.size());
  std::memcpy(snapshot.data(), &header, sizeof(header));
  return snapshot;
}

std::vector<std::uint8_t> BuildValidZoomCommandFrame() {
  using namespace ntium::ipc;

  std::vector<std::uint8_t> payload(8 + 16, 0);
  WriteU16Le(payload.data(), 0, static_cast<std::uint16_t>(PipeCommandId::kZoom));
  WriteU8(payload.data(), 2, 1);   // command_version
  WriteU8(payload.data(), 3, 0);   // command_flags
  WriteU32Le(payload.data(), 4, 0);  // reserved

  WriteF32Le(payload.data(), 8, 0.5f);   // delta
  WriteF32Le(payload.data(), 12, 0.5f);  // anchor x
  WriteF32Le(payload.data(), 16, 0.5f);  // anchor y
  WriteU8(payload.data(), 20, 0);        // dolly
  WriteU8(payload.data(), 21, 1);        // clamp_to_surface
  WriteU16Le(payload.data(), 22, 0);     // reserved

  PipeFrameHeader header{};
  header.msg_type = PipeMessageType::kCommand;
  header.sequence = 1;
  header.ack_sequence = 0;
  header.flags = static_cast<std::uint8_t>(PipeHeaderFlags::kAckRequired);
  header.payload_len = static_cast<std::uint32_t>(payload.size());
  header.payload_crc32c = Crc32c(payload);

  const std::array<std::uint8_t, kPipeHeaderBytes> header_bytes =
      EncodeFrameHeader(header);
  std::vector<std::uint8_t> frame;
  frame.insert(frame.end(), header_bytes.begin(), header_bytes.end());
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

double Distance(const scene::Vec3d& a, const scene::Vec3d& b) {
  const scene::Vec3d d{a.x - b.x, a.y - b.y, a.z - b.z};
  return scene::Length(d);
}

scene::Vec3d ForwardFromAngles(double yaw_rad, double pitch_rad) {
  const double cos_pitch = std::cos(pitch_rad);
  return scene::Normalize(scene::Vec3d{
      std::cos(yaw_rad) * cos_pitch,
      std::sin(yaw_rad) * cos_pitch,
      std::sin(pitch_rad),
  });
}

CheckResult TestSnapshotParser() {
  using namespace ntium::ingest;
  CheckResult out{"snapshot-parser-valid-and-invalid", false, ""};

  const std::vector<std::uint8_t> valid_snapshot = BuildValidSnapshot();
  const SnapshotParseContext ctx{.last_applied_sequence = 0};
  const SnapshotParseResult ok = ParseSnapshotV1(valid_snapshot, ctx);
  if (ok.disposition != SnapshotDisposition::kApply ||
      ok.error != SnapshotParseError::kNone || ok.records.size() != 1) {
    out.detail = "valid snapshot was not accepted";
    return out;
  }

  std::vector<std::uint8_t> invalid = valid_snapshot;
  invalid.back() ^= 0x01;
  const SnapshotParseResult bad = ParseSnapshotV1(invalid, ctx);
  if (bad.disposition != SnapshotDisposition::kRejectSnapshot ||
      bad.error == SnapshotParseError::kNone) {
    out.detail = "invalid snapshot was not rejected";
    return out;
  }

  out.pass = true;
  out.detail = "accepted valid snapshot and rejected corrupted one";
  return out;
}

CheckResult TestIpcProtocol() {
  using namespace ntium::ipc;
  CheckResult out{"ipc-frame-parse-and-validation", false, ""};

  std::vector<std::uint8_t> frame = BuildValidZoomCommandFrame();
  PipeFrame parsed{};
  PipeValidationIssue issue{};
  if (!TryParseFrame(frame, &parsed, &issue)) {
    out.detail = "valid frame failed to parse";
    return out;
  }

  // Corrupt anchor_x to force payload validation failure.
  std::vector<std::uint8_t> bad = frame;
  WriteF32Le(bad.data(), kPipeHeaderBytes + 12, 2.0f);
  std::vector<std::uint8_t> bad_payload(bad.begin() + kPipeHeaderBytes, bad.end());
  ntium::ipc::WriteU32Le(
      bad.data(),
      24,
      Crc32c(std::span<const std::uint8_t>(bad_payload.data(), bad_payload.size())));
  PipeFrameHeader updated_header{};
  if (!TryDecodeFrameHeader(std::span<const std::uint8_t>(bad.data(), bad.size()),
                            &updated_header)) {
    out.detail = "failed to decode header during invalid test";
    return out;
  }
  updated_header.payload_crc32c = Crc32c(
      std::span<const std::uint8_t>(bad_payload.data(), bad_payload.size()));
  const auto new_header = EncodeFrameHeader(updated_header);
  std::copy(new_header.begin(), new_header.end(), bad.begin());

  PipeFrame unused{};
  PipeValidationIssue bad_issue{};
  if (TryParseFrame(bad, &unused, &bad_issue)) {
    out.detail = "invalid zoom anchor unexpectedly passed";
    return out;
  }
  if (bad_issue.code != PipeErrorCode::kMalformedPayload) {
    out.detail = "invalid zoom anchor did not emit malformed payload";
    return out;
  }

  out.pass = true;
  out.detail = "valid command accepted and malformed command rejected";
  return out;
}

CheckResult TestCameraZoom() {
  CheckResult out{"camera-cursor-anchored-zoom", false, ""};

  scene::CameraController camera;
  scene::CameraState seeded{};
  seeded.mode = scene::CameraMode::kOrbit;
  seeded.position_ecef = scene::Vec3d{0.0, -7'000'000.0, 0.0};
  seeded.yaw_rad = 1.5707963267948966;  // +Y
  seeded.pitch_rad = 0.0;
  seeded.orbit_radius_m = 1'000'000.0;
  seeded.orbit_target_ecef = scene::Vec3d{0.0, 0.0, 0.0};
  camera.SetState(seeded);
  scene::CameraState state = camera.state();
  const scene::Vec3d forward_initial =
      ForwardFromAngles(state.yaw_rad, state.pitch_rad);

  const scene::RayEllipsoidForwardHit hit = scene::ClosestForwardRayEllipsoidHit(
      state.position_ecef,
      forward_initial,
      scene::Wgs84Ellipsoid());
  if (!hit.hit) {
    const scene::RayEllipsoidIntersection debug = scene::IntersectRayWithEllipsoid(
        state.position_ecef, forward_initial, scene::Wgs84Ellipsoid());
    out.detail = "failed to find initial ray/ellipsoid hit (intersects=" +
                 std::string(debug.intersects ? "true" : "false") +
                 ", t_near=" + std::to_string(debug.t_near) +
                 ", t_far=" + std::to_string(debug.t_far) + ")";
    return out;
  }

  const double before = Distance(state.position_ecef, hit.point);
  scene::CameraTickInput tick{};
  tick.orbit.zoom_delta = 5000.0;
  tick.orbit.has_cursor_ray = true;
  tick.orbit.cursor_ray_origin_ecef = state.position_ecef;
  tick.orbit.cursor_ray_direction_ecef = forward_initial;
  camera.Tick(1.0, tick);
  state = camera.state();
  const double after = Distance(state.position_ecef, hit.point);
  if (!(after < before)) {
    out.detail = "anchor dolly did not reduce distance to anchor";
    return out;
  }

  const scene::CameraState before_fallback = camera.state();
  const scene::Vec3d forward =
      ForwardFromAngles(before_fallback.yaw_rad, before_fallback.pitch_rad);
  scene::CameraTickInput fallback{};
  fallback.orbit.zoom_delta = 1000.0;
  fallback.orbit.has_cursor_ray = true;
  fallback.orbit.cursor_ray_origin_ecef = before_fallback.position_ecef;
  fallback.orbit.cursor_ray_direction_ecef = scene::Vec3d{0.0, -1.0, 0.0};  // miss
  camera.Tick(1.0, fallback);
  const scene::CameraState after_fallback = camera.state();
  const scene::Vec3d delta{
      after_fallback.position_ecef.x - before_fallback.position_ecef.x,
      after_fallback.position_ecef.y - before_fallback.position_ecef.y,
      after_fallback.position_ecef.z - before_fallback.position_ecef.z};
  const double projected = scene::Dot(delta, forward);
  if (!(projected > 0.0)) {
    out.detail = "fallback zoom did not move camera forward";
    return out;
  }

  out.pass = true;
  out.detail = "anchor hit dolly and no-hit fallback both behaved as expected";
  return out;
}

CheckResult TestOverlayAndDirtyPipeline() {
  CheckResult out{"overlay-store-throttle-dirty-coalescer", false, ""};

  overlay::EventStore store;
  const auto now = overlay::EventStore::Clock::now();
  if (store.Upsert("event-a", overlay::OverlayBounds{10, 10, 50, 50}, 10,
                   std::chrono::milliseconds(1000), now) !=
      overlay::EventStoreResult::kApplied) {
    out.detail = "failed to upsert event-a";
    return out;
  }
  if (store.Upsert("event-a", overlay::OverlayBounds{11, 11, 51, 51}, 9,
                   std::chrono::milliseconds(1000), now) !=
      overlay::EventStoreResult::kIgnoredStale) {
    out.detail = "stale upsert was not ignored";
    return out;
  }
  if (store.Clear("event-a", 11) != overlay::EventStoreResult::kApplied) {
    out.detail = "clear did not apply";
    return out;
  }
  if (store.Upsert("event-a", overlay::OverlayBounds{12, 12, 52, 52}, 10,
                   std::chrono::milliseconds(1000), now) !=
      overlay::EventStoreResult::kIgnoredStale) {
    out.detail = "tombstone stale check failed";
    return out;
  }

  if (store.Upsert("event-b", overlay::OverlayBounds{100, 100, 120, 120}, 12,
                   std::chrono::milliseconds(5), now) !=
      overlay::EventStoreResult::kApplied) {
    out.detail = "failed to upsert event-b";
    return out;
  }
  const auto expired = store.Expire(now + std::chrono::milliseconds(20));
  if (expired.size() != 1 || expired[0].event_id != "event-b") {
    out.detail = "ttl expiration failed";
    return out;
  }

  overlay::DirtyRegionDelta delta{};
  delta.event_id = "move-1";
  delta.event_sequence = 13;
  delta.old_bounds = overlay::OverlayBounds{0, 0, 20, 20};
  delta.new_bounds = overlay::OverlayBounds{40, 40, 60, 60};
  delta.padding_px = 2;
  const auto dirty = overlay::DirtyRegionGenerator::Generate(delta);
  if (dirty.size() < 2) {
    out.detail = "dirty region generator did not produce expected split regions";
    return out;
  }

  overlay::OverlayThrottle throttle(std::chrono::milliseconds(100));
  if (!throttle.RecordOverlayInvalidation(100)) {
    out.detail = "failed to record initial overlay invalidation";
    return out;
  }
  const auto t0 = Clock::now();
  auto d0 = throttle.Evaluate(t0, true, false);
  if (!d0.allow_present) {
    out.detail = "initial throttle evaluation should allow present";
    return out;
  }
  throttle.OnPresented(t0, d0.coalesced_sequence);
  throttle.RecordOverlayInvalidation(101);
  auto d1 = throttle.Evaluate(t0 + std::chrono::milliseconds(50), true, false);
  if (d1.allow_present || !d1.throttled) {
    out.detail = "overlay-only present should be throttled inside interval";
    return out;
  }
  auto d2 = throttle.Evaluate(t0 + std::chrono::milliseconds(101), true, false);
  if (!d2.allow_present) {
    out.detail = "overlay present should resume after interval";
    return out;
  }

  auto coalescer = render::CreateDirtyRectCoalescer();
  render::MonitorLayout m0{};
  m0.monitor.adapter.luid_low_part = 1;
  m0.monitor.adapter.luid_high_part = 0;
  m0.monitor.monitor_key = "m0";
  m0.monitor.target_id = 0;
  m0.desktop_left = 0;
  m0.desktop_top = 0;
  m0.width = 1920;
  m0.height = 1080;
  m0.is_primary = true;
  render::MonitorLayout m1 = m0;
  m1.monitor.monitor_key = "m1";
  m1.monitor.target_id = 1;
  m1.desktop_left = 1920;
  m1.width = 2560;
  m1.height = 1440;
  m1.is_primary = false;

  render::DirtyPresentEvent event{};
  event.event_sequence = 200;
  event.reason = render::PresentReason::kInvalidation;
  event.monitor_target = render::DirtyMonitorTarget::kAllActiveMonitors;
  event.coord_space = render::DirtyRectCoordSpace::kDesktopPixels;
  event.dirty_rects.push_back(render::DirtyRectPx{1900, 100, 1940, 140});

  const auto map_result = coalescer->Coalesce(event, {m0, m1});
  if (!map_result.has_effective_work || map_result.per_monitor.size() != 2) {
    out.detail = "coalescer did not map desktop rect across monitors";
    return out;
  }

  out.pass = true;
  out.detail =
      "event store, throttle, dirty generation, and monitor coalescing succeeded";
  return out;
}

CheckResult TestInputAndArbitration() {
  CheckResult out{"input-router-and-arbitration", false, ""};

  ntium::input::InputRouter router;
  if (router.SetMode(ntium::input::InputMode::kActive,
                     ntium::input::InputTransitionReason::kUserRequest)) {
    out.detail = "router should not activate without focus";
    return out;
  }
  router.OnFocusChanged(true, ntium::input::InputTransitionReason::kFocusGained);
  if (!router.SetMode(ntium::input::InputMode::kActive,
                      ntium::input::InputTransitionReason::kUserRequest)) {
    out.detail = "router failed to enter active mode";
    return out;
  }
  if (!router.BeginInteraction(ntium::input::InputTransitionReason::kUserRequest)) {
    out.detail = "router failed to begin interaction";
    return out;
  }

  ntium::input::ArbitrationEngine arbitration;
  const std::uint64_t now_ms = 1000;
  arbitration.AcquireOrRenewUserLease(router.snapshot().interaction_epoch, now_ms);

  ntium::input::ArbitrationRequest request{};
  request.source = ntium::input::ArbitrationSource::kIpc;
  request.channel = ntium::input::ArbitrationChannel::kCameraRotate;
  request.source_sequence = 1;
  request.monotonic_time_ms = now_ms + 1;
  request.allow_deferred_coalescing = true;
  auto decision = arbitration.Evaluate(request);
  if (decision.should_execute_now || decision.disposition == ntium::input::ArbitrationDisposition::kRejected) {
    out.detail = "ipc request should defer while user lease is active";
    return out;
  }

  if (!arbitration.ReleaseUserLease(router.snapshot().interaction_epoch, now_ms + 10)) {
    out.detail = "failed to release user lease";
    return out;
  }
  const auto drained = arbitration.DrainDeferred(now_ms + 20);
  if (drained.size() != 1) {
    out.detail = "expected one deferred command after lease release";
    return out;
  }

  out.pass = true;
  out.detail = "input focus rules and ipc defer/drain arbitration behaved correctly";
  return out;
}

void PrintResults(const std::vector<CheckResult>& results) {
  std::cout << "Live Validation Demo Results\n";
  std::cout << "============================\n";
  for (const CheckResult& r : results) {
    std::cout << (r.pass ? "[PASS] " : "[FAIL] ") << r.name << " :: " << r.detail
              << "\n";
  }
  const std::size_t passed =
      static_cast<std::size_t>(std::count_if(results.begin(), results.end(),
                                             [](const CheckResult& r) { return r.pass; }));
  std::cout << "----------------------------\n";
  std::cout << "Passed " << passed << " / " << results.size() << " checks\n";
}

bool WriteJsonReport(const std::string& path, const std::vector<CheckResult>& results) {
  std::ofstream out(path);
  if (!out) {
    return false;
  }
  const auto now = std::chrono::system_clock::now();
  const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                         now.time_since_epoch())
                         .count();
  out << "{\n";
  out << "  \"timestamp_unix_s\": " << now_s << ",\n";
  out << "  \"checks\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    out << "    {\n";
    out << "      \"name\": \"" << r.name << "\",\n";
    out << "      \"pass\": " << (r.pass ? "true" : "false") << ",\n";
    out << "      \"detail\": \"" << r.detail << "\"\n";
    out << "    }";
    if (i + 1 < results.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::optional<std::string> json_out;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--json-out" && i + 1 < argc) {
      json_out = std::string(argv[++i]);
    }
  }

  std::vector<CheckResult> results;
  results.push_back(TestSnapshotParser());
  results.push_back(TestIpcProtocol());
  results.push_back(TestCameraZoom());
  results.push_back(TestOverlayAndDirtyPipeline());
  results.push_back(TestInputAndArbitration());

  PrintResults(results);

  if (json_out.has_value()) {
    if (!WriteJsonReport(*json_out, results)) {
      std::cerr << "Failed to write JSON report to " << *json_out << "\n";
    } else {
      std::cout << "JSON report: " << *json_out << "\n";
    }
  }

  const bool all_pass = std::all_of(results.begin(), results.end(),
                                    [](const CheckResult& r) { return r.pass; });
  return all_pass ? 0 : 1;
}
