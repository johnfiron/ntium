#include "src/app/AppHost.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace ntium::app {
namespace {

constexpr std::uint32_t kAckAppliedFlag = 0x1U;
constexpr std::uint32_t kAckCoalescedFlag = 0x2U;
constexpr std::uint32_t kAckDeferredFlag = 0x4U;
constexpr std::size_t kCommandEnvelopeBytes = 8U;

AppHostResult MakeAppHostResult(
    AppHostStatusCode code, std::string stage, std::string detail) {
  AppHostResult result;
  result.code = code;
  result.stage = std::move(stage);
  result.detail = std::move(detail);
  return result;
}

RuntimeGraphResult MakeInitFailure(std::string node, std::string detail) {
  return MakeRuntimeGraphResult(
      RuntimeGraphStatusCode::kInitializeFailed, std::move(node), std::move(detail));
}

overlay::EventStore::Clock::time_point EventStoreTimeFromMs(std::uint64_t now_ms) {
  return overlay::EventStore::Clock::time_point(std::chrono::milliseconds(now_ms));
}

overlay::OverlayThrottle::Clock::time_point OverlayTimeFromMs(std::uint64_t now_ms) {
  return overlay::OverlayThrottle::Clock::time_point(std::chrono::milliseconds(now_ms));
}

render::DirtyRectPx ToDirtyRectPx(const overlay::DirtyRegionRect& rect) {
  return render::DirtyRectPx{
      .left = rect.left,
      .top = rect.top,
      .right = rect.right,
      .bottom = rect.bottom,
  };
}

overlay::OverlayBounds BoundsFromPoint(std::int32_t x, std::int32_t y, std::int32_t size_px) {
  const std::int32_t half = std::max<std::int32_t>(1, size_px / 2);
  return overlay::OverlayBounds{
      .left = x - half,
      .top = y - half,
      .right = x + half,
      .bottom = y + half,
  };
}

overlay::OverlayBounds BoundsFromRing(
    std::int32_t center_x, std::int32_t center_y, std::int32_t radius) {
  return overlay::OverlayBounds{
      .left = center_x - radius,
      .top = center_y - radius,
      .right = center_x + radius,
      .bottom = center_y + radius,
  };
}

std::chrono::milliseconds ThrottleIntervalFromHz(std::uint32_t hz) {
  if (hz == 0U) {
    return std::chrono::milliseconds(100);
  }
  const auto interval_ms = static_cast<std::uint64_t>(1000U / hz);
  return std::chrono::milliseconds(
      interval_ms == 0U ? 1U : interval_ms);
}

std::vector<std::uint8_t> ReadBinaryFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  stream.seekg(0, std::ios::end);
  const std::streamsize size = stream.tellg();
  if (size <= 0) {
    return {};
  }
  stream.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
  stream.read(reinterpret_cast<char*>(buffer.data()), size);
  if (!stream) {
    return {};
  }
  return buffer;
}

}  // namespace

const char* ToString(AppHostStatusCode code) {
  switch (code) {
    case AppHostStatusCode::kOk:
      return "ok";
    case AppHostStatusCode::kInvalidState:
      return "invalid_state";
    case AppHostStatusCode::kBootstrapFailed:
      return "bootstrap_failed";
    case AppHostStatusCode::kShutdownFailed:
      return "shutdown_failed";
  }
  return "unknown";
}

AppHost::AppHost(AppHostConfig config)
    : config_(std::move(config)),
      overlay_throttle_(ThrottleIntervalFromHz(config_.overlay_throttle_hz)) {}

AppHostResult AppHost::Initialize() {
  if (initialized_) {
    return MakeAppHostResult(
        AppHostStatusCode::kInvalidState, "initialize",
        "AppHost is already initialized");
  }

  bootstrap_order_.clear();
  shutdown_order_.clear();
  notes_.clear();
  telemetry_ = {};
  runtime_event_sequence_ = 0U;
  last_snapshot_sequence_ = 0U;
  last_snapshot_generation_ = 0U;
  active_pipe_connection_id_ = 0U;
  control_state_revision_ = 0U;
  overlay_only_deadline_ms_ = 0U;
  overlay_record_types_.clear();
  active_monitors_.clear();

  if (!runtime_graph_built_) {
    const AppHostResult build_result = BuildRuntimeGraph();
    if (!build_result.ok()) {
      return build_result;
    }
  }

  const RuntimeGraphResult graph_result = runtime_graph_.Initialize();
  const AppHostResult app_result = ConvertResult(graph_result, false);
  if (!app_result.ok()) {
    initialized_ = false;
    return app_result;
  }

  initialized_ = true;
  PollRuntimeFeeds(NowMs());
  return MakeAppHostResult(AppHostStatusCode::kOk, "initialize", {});
}

AppHostResult AppHost::Shutdown() {
  if (!runtime_graph_built_) {
    return MakeAppHostResult(AppHostStatusCode::kOk, "shutdown", {});
  }

  const RuntimeGraphResult graph_result = runtime_graph_.Shutdown();
  initialized_ = false;
  return ConvertResult(graph_result, true);
}

#if defined(_WIN32)
bool AppHost::OnWindowMessage(
    std::uint32_t message,
    std::uintptr_t wparam,
    std::intptr_t lparam,
    std::intptr_t* out_result) {
  if (!initialized_) {
    return false;
  }

  if (message == WM_WTSSESSION_CHANGE && session_manager_ != nullptr) {
    (void)session_manager_->HandleSessionChange(
        static_cast<std::uint32_t>(wparam), lparam, NowMs());
  } else if (message == WM_POWERBROADCAST && session_manager_ != nullptr) {
    (void)session_manager_->HandlePowerBroadcast(
        static_cast<std::uint32_t>(wparam), NowMs());
  } else if (message == WM_ENDSESSION && session_manager_ != nullptr) {
    (void)session_manager_->HandleEndSession(wparam != 0U, NowMs());
  }

  if (desktop_host_ != nullptr &&
      desktop_host_->DispatchWindowMessage(message, wparam, lparam)) {
    if (out_result != nullptr) {
      *out_result = 0;
    }
    return true;
  }

  if (input_router_ != nullptr && arbitration_engine_ != nullptr) {
    const std::uint64_t now_ms = NowMs();
    switch (message) {
      case WM_SETFOCUS:
        (void)input_router_->OnFocusChanged(
            true, input::InputTransitionReason::kFocusGained);
        break;
      case WM_KILLFOCUS:
        (void)input_router_->OnFocusChanged(
            false, input::InputTransitionReason::kFocusLost);
        arbitration_engine_->CancelUserLeaseForSystem(now_ms);
        break;
      case WM_LBUTTONDOWN:
        (void)input_router_->SetMode(
            input::InputMode::kActive, input::InputTransitionReason::kUserRequest);
        (void)input_router_->AcquirePointerCapture(
            input::InputTransitionReason::kCaptureAcquired);
        if (input_router_->BeginInteraction(input::InputTransitionReason::kUserRequest)) {
          arbitration_engine_->AcquireOrRenewUserLease(
              input_router_->snapshot().interaction_epoch, now_ms);
        }
        last_pointer_position_px_ = std::make_pair(
            static_cast<std::int32_t>(GET_X_LPARAM(lparam)),
            static_cast<std::int32_t>(GET_Y_LPARAM(lparam)));
        break;
      case WM_MOUSEMOVE: {
        const std::int32_t x = static_cast<std::int32_t>(GET_X_LPARAM(lparam));
        const std::int32_t y = static_cast<std::int32_t>(GET_Y_LPARAM(lparam));
        if ((wparam & MK_LBUTTON) != 0U && last_pointer_position_px_.has_value() &&
            input_router_->snapshot().interaction_active) {
          const float delta_x = static_cast<float>(x - last_pointer_position_px_->first);
          const float delta_y = static_cast<float>(y - last_pointer_position_px_->second);
          if (ApplyCameraRotateCommand(delta_x * 0.2F, delta_y * 0.2F)) {
            overlay::DirtyRegionRect dirty{};
            dirty.left = x - 2;
            dirty.top = y - 2;
            dirty.right = x + 2;
            dirty.bottom = y + 2;
            (void)DispatchDirtyPresentEvent(
                std::vector<overlay::DirtyRegionRect>{dirty},
                render::PresentReason::kInvalidation,
                false,
                true,
                NextEventSequence());
          }
        }
        last_pointer_position_px_ = std::make_pair(x, y);
        break;
      }
      case WM_LBUTTONUP:
        (void)input_router_->ReleasePointerCapture(
            input::InputTransitionReason::kCaptureReleased);
        (void)input_router_->EndInteraction(input::InputTransitionReason::kUserRequest);
        (void)arbitration_engine_->ReleaseUserLease(
            input_router_->snapshot().interaction_epoch, now_ms);
        break;
      case WM_MOUSEWHEEL:
        if (ApplyCameraZoomCommand(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)))) {
          (void)DispatchDirtyPresentEvent(
              {},
              render::PresentReason::kInvalidation,
              false,
              true,
              NextEventSequence());
        }
        break;
      default:
        break;
    }
  }

  PollRuntimeFeeds(NowMs());
  return false;
}
#endif

AppHostResult AppHost::BuildRuntimeGraph() {
  runtime_graph_ = RuntimeGraph();

  const RuntimeGraphNode startup_node{
      .name = "startup_manager",
      .initialize = [this]() { return InitializeStartupManager(); },
      .shutdown = [this]() { return ShutdownStartupManager(); },
  };
  RuntimeGraphResult result = runtime_graph_.AddNode(startup_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode session_node{
      .name = "session_manager",
      .initialize = [this]() { return InitializeSessionManager(); },
      .shutdown = [this]() { return ShutdownSessionManager(); },
  };
  result = runtime_graph_.AddNode(session_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode desktop_node{
      .name = "desktop_host",
      .initialize = [this]() { return InitializeDesktopHost(); },
      .shutdown = [this]() { return ShutdownDesktopHost(); },
  };
  result = runtime_graph_.AddNode(desktop_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode monitor_node{
      .name = "monitor_manager",
      .initialize = [this]() { return InitializeMonitorManager(); },
      .shutdown = [this]() { return ShutdownMonitorManager(); },
  };
  result = runtime_graph_.AddNode(monitor_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode render_node{
      .name = "render_runtime",
      .initialize = [this]() { return InitializeRenderRuntime(); },
      .shutdown = [this]() { return ShutdownRenderRuntime(); },
  };
  result = runtime_graph_.AddNode(render_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode input_node{
      .name = "input_runtime",
      .initialize = [this]() { return InitializeInputRuntime(); },
      .shutdown = [this]() { return ShutdownInputRuntime(); },
  };
  result = runtime_graph_.AddNode(input_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode ipc_node{
      .name = "ipc_runtime",
      .initialize = [this]() { return InitializeIpcRuntime(); },
      .shutdown = [this]() { return ShutdownIpcRuntime(); },
  };
  result = runtime_graph_.AddNode(ipc_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode ingest_node{
      .name = "ingest_runtime",
      .initialize = [this]() { return InitializeIngestRuntime(); },
      .shutdown = [this]() { return ShutdownIngestRuntime(); },
  };
  result = runtime_graph_.AddNode(ingest_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  runtime_graph_built_ = true;
  return MakeAppHostResult(AppHostStatusCode::kOk, "build_runtime_graph", {});
}

AppHostResult AppHost::ConvertResult(
    RuntimeGraphResult result, bool shutdown_path) const {
  if (result.ok()) {
    return MakeAppHostResult(
        AppHostStatusCode::kOk, shutdown_path ? "shutdown" : "initialize", {});
  }
  return MakeAppHostResult(
      shutdown_path ? AppHostStatusCode::kShutdownFailed
                    : AppHostStatusCode::kBootstrapFailed,
      result.node_name, std::move(result.detail));
}

std::uint64_t AppHost::NowMs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

RuntimeGraphResult AppHost::InitializeStartupManager() {
  startup_manager_ = std::make_unique<runtime::StartupManager>();
  const runtime::StartupDecision startup_decision =
      startup_manager_->EvaluateStartup(
          runtime::StartupContext{
              .launch_reason = runtime::StartupLaunchReason::kProcessStart,
              .now_ms = NowMs(),
          });
  if (startup_decision.disposition ==
      runtime::StartupDecisionDisposition::kDenyDuplicateInstance) {
    return MakeInitFailure(
        "startup_manager", "single-instance gate rejected duplicate host instance");
  }
  if (startup_decision.disposition ==
      runtime::StartupDecisionDisposition::kDenyPolicySuppressed) {
    return MakeInitFailure(
        "startup_manager", "startup policy suppressed initialization");
  }
  startup_manager_->MarkProcessRunning(NowMs());
  bootstrap_order_.push_back("startup_manager");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::ShutdownStartupManager() {
  if (startup_manager_ != nullptr) {
    startup_manager_->OnProcessExit(runtime::ExitContext{
        .reason = runtime::StartupExitReason::kGracefulShutdown,
        .now_ms = NowMs(),
    });
    startup_manager_->ReleaseSingleInstance();
    startup_manager_.reset();
  }
  shutdown_order_.push_back("startup_manager");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeSessionManager() {
  runtime::SessionTransitionHooks hooks;
#if defined(_WIN32)
  hooks.rebind_desktop = [this](runtime::SessionTransitionTrigger /*trigger*/,
                                std::uint64_t /*now_ms*/) {
    if (desktop_host_ == nullptr) {
      return true;
    }
    return desktop_host_->EnsureAttached(
        platform::win32::HostRebindReason::kSessionChanged);
  };
#endif
  session_manager_ = std::make_unique<runtime::SessionManager>(std::move(hooks));
  (void)session_manager_->ProcessTrigger(
      runtime::SessionTransitionTrigger::kProcessStart, NowMs());
  bootstrap_order_.push_back("session_manager");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::ShutdownSessionManager() {
  if (session_manager_ != nullptr) {
    (void)session_manager_->ProcessTrigger(
        runtime::SessionTransitionTrigger::kShutdown, NowMs());
    session_manager_.reset();
  }
  shutdown_order_.push_back("session_manager");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeDesktopHost() {
#if defined(_WIN32)
  desktop_host_ = std::make_unique<platform::win32::DesktopHost>();
  if (!desktop_host_->Initialize()) {
    return MakeInitFailure("desktop_host", "DesktopHost::Initialize failed");
  }
  bootstrap_order_.push_back("desktop_host");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
#else
  notes_.push_back("desktop_host: skipped on non-Windows host");
  bootstrap_order_.push_back("desktop_host(stub)");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
#endif
}

RuntimeGraphResult AppHost::ShutdownDesktopHost() {
#if defined(_WIN32)
  if (desktop_host_ != nullptr) {
    desktop_host_->Shutdown();
    desktop_host_.reset();
  }
#endif
  shutdown_order_.push_back("desktop_host");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeMonitorManager() {
#if defined(_WIN32)
  monitor_manager_ = std::make_unique<platform::win32::MonitorManager>();
  if (!monitor_manager_->Initialize()) {
    return MakeInitFailure(
        "monitor_manager", "MonitorManager::Initialize failed");
  }
  bootstrap_order_.push_back("monitor_manager");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
#else
  notes_.push_back("monitor_manager: skipped on non-Windows host");
  bootstrap_order_.push_back("monitor_manager(stub)");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
#endif
}

RuntimeGraphResult AppHost::ShutdownMonitorManager() {
#if defined(_WIN32)
  if (monitor_manager_ != nullptr) {
    monitor_manager_->Shutdown();
    monitor_manager_.reset();
  }
#endif
  shutdown_order_.push_back("monitor_manager");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeRenderRuntime() {
  render_device_manager_ = render::CreateRenderDeviceManager();
  swapchain_manager_ = render::CreateSwapchainManager();
  dirty_present_pipeline_ = render::CreateDirtyPresentPipeline();
  device_recovery_ = render::CreateDeviceRecoveryStateMachine();

  if (!render_device_manager_ || !swapchain_manager_ ||
      !dirty_present_pipeline_ || !device_recovery_) {
    return MakeInitFailure(
        "render_runtime", "failed to create render runtime managers");
  }

  notes_.push_back(
      "render_runtime: TODO A2 follow-up for adapter enumeration + swapchain binding");
  bootstrap_order_.push_back("render_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::ShutdownRenderRuntime() {
  device_recovery_.reset();
  dirty_present_pipeline_.reset();
  swapchain_manager_.reset();
  render_device_manager_.reset();
  shutdown_order_.push_back("render_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeInputRuntime() {
  input_router_ = std::make_unique<input::InputRouter>();
  arbitration_engine_ = std::make_unique<input::ArbitrationEngine>();
  if (!input_router_ || !arbitration_engine_) {
    return MakeInitFailure(
        "input_runtime", "failed to create input runtime components");
  }

  notes_.push_back(
      "input_runtime: TODO integrate live camera command dispatch in later batch");
  bootstrap_order_.push_back("input_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::ShutdownInputRuntime() {
  arbitration_engine_.reset();
  input_router_.reset();
  shutdown_order_.push_back("input_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeIpcRuntime() {
  ipc::PipeServerConfig pipe_config;
  pipe_server_ = ipc::PipeServer::Create(std::move(pipe_config));
  if (!pipe_server_) {
    return MakeInitFailure("ipc_runtime", "PipeServer::Create returned nullptr");
  }

  const ipc::PipeServerError start_error = pipe_server_->Start();
  if (start_error == ipc::PipeServerError::kNone) {
    ipc_started_ = true;
    bootstrap_order_.push_back("ipc_runtime");
    return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
  }

  ipc_started_ = false;
  if (start_error == ipc::PipeServerError::kPlatformUnsupported &&
      !config_.require_ipc_server) {
    notes_.push_back("ipc_runtime: running in platform-stub mode");
    bootstrap_order_.push_back("ipc_runtime(stub)");
    return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
  }

  return MakeInitFailure(
      "ipc_runtime",
      "TODO not implemented: PipeServer runtime loop/transport binding required");
}

RuntimeGraphResult AppHost::ShutdownIpcRuntime() {
  if (pipe_server_ != nullptr) {
    pipe_server_->Stop();
    pipe_server_.reset();
  }
  ipc_started_ = false;
  shutdown_order_.push_back("ipc_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeIngestRuntime() {
  overlay_event_store_ = std::make_unique<overlay::EventStore>();
  if (!overlay_event_store_) {
    return MakeInitFailure("ingest_runtime", "failed to create overlay event store");
  }

  ingest::SnapshotWatcherConfig watcher_config;
  watcher_config.watch_directory = config_.snapshot_watch_directory;
  watcher_config.snapshot_filename_filter = config_.snapshot_filename_filter;

  if (watcher_config.watch_directory.empty()) {
    if (config_.require_snapshot_watcher) {
      return MakeInitFailure(
          "ingest_runtime",
          "TODO not implemented: snapshot_watch_directory is required but missing");
    }
    notes_.push_back(
        "ingest_runtime: TODO configure snapshot_watch_directory when enabling live ingest");
  } else {
    snapshot_watcher_ = std::make_unique<ingest::SnapshotWatcher>(watcher_config);
    const ingest::SnapshotWatcherError watcher_error = snapshot_watcher_->Start();
    if (watcher_error == ingest::SnapshotWatcherError::kNone) {
      snapshot_watcher_started_ = true;
    } else if (watcher_error == ingest::SnapshotWatcherError::kUnsupportedPlatform &&
               !config_.require_snapshot_watcher) {
      notes_.push_back("ingest_runtime: watcher unsupported on current platform");
    } else if (config_.require_snapshot_watcher) {
      return MakeInitFailure(
          "ingest_runtime",
          "TODO not implemented: native snapshot watcher runtime integration failed");
    } else {
      notes_.push_back("ingest_runtime: snapshot watcher failed to start; continuing");
    }
  }

  bootstrap_order_.push_back("ingest_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::ShutdownIngestRuntime() {
  if (snapshot_watcher_ != nullptr) {
    if (snapshot_watcher_started_) {
      (void)snapshot_watcher_->Stop();
      snapshot_watcher_started_ = false;
    }
    snapshot_watcher_.reset();
  }
  overlay_event_store_.reset();
  shutdown_order_.push_back("ingest_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::RefreshRenderTopology(std::uint64_t event_sequence) {
  if (dirty_present_pipeline_ == nullptr || swapchain_manager_ == nullptr ||
      render_device_manager_ == nullptr) {
    return MakeInitFailure("render_runtime", "render pipeline is not initialized");
  }

  std::vector<render::MonitorLayout> next_layout;
  next_layout.reserve(active_monitors_.size() + 1U);

#if defined(_WIN32)
  if (monitor_manager_ != nullptr) {
    const platform::win32::MonitorTopologySnapshot topology =
        monitor_manager_->topology_snapshot();
    for (const auto& entry : topology.monitors) {
      render::MonitorLayout layout{};
      layout.monitor.adapter.luid_low_part = entry.identity.adapter_luid_low;
      layout.monitor.adapter.luid_high_part = entry.identity.adapter_luid_high;
      layout.monitor.adapter.adapter_key = std::to_string(entry.identity.adapter_luid_low) +
                                           ":" +
                                           std::to_string(entry.identity.adapter_luid_high);
      layout.monitor.target_id = entry.identity.target_id;
      layout.monitor.monitor_key = entry.identity.monitor_key;
      layout.desktop_left = entry.geometry.left;
      layout.desktop_top = entry.geometry.top;
      layout.width = entry.geometry.width;
      layout.height = entry.geometry.height;
      layout.is_primary = entry.is_primary;
      next_layout.push_back(layout);
    }
  }
#endif

  if (next_layout.empty()) {
    render::MonitorLayout fallback{};
    fallback.monitor.adapter.luid_low_part = 1U;
    fallback.monitor.adapter.luid_high_part = 0;
    fallback.monitor.adapter.adapter_key = "fallback-adapter";
    fallback.monitor.target_id = 1U;
    fallback.monitor.monitor_key = "fallback-monitor";
    fallback.desktop_left = 0;
    fallback.desktop_top = 0;
    fallback.width = 1920U;
    fallback.height = 1080U;
    fallback.is_primary = true;
    next_layout.push_back(fallback);
  }

  active_monitors_ = next_layout;
  for (const auto& monitor : active_monitors_) {
    (void)render_device_manager_->EnsureDeviceForAdapter(monitor.monitor.adapter);
    render::SwapchainDescriptor descriptor{};
    descriptor.width = monitor.width;
    descriptor.height = monitor.height;
    descriptor.buffer_count = 2U;
    descriptor.allow_tearing = false;
    (void)swapchain_manager_->EnsureSwapchainForMonitor(
        monitor.monitor, descriptor, event_sequence);
    (void)device_recovery_->EnsureTrackedAdapter(monitor.monitor.adapter, event_sequence);
  }

  const render::RenderStatus status =
      dirty_present_pipeline_->SetActiveMonitors(active_monitors_, event_sequence);
  if (status != render::RenderStatus::kOk) {
    return MakeInitFailure("render_runtime", "failed to apply active monitor topology");
  }
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::DispatchDirtyPresentEvent(
    const std::vector<overlay::DirtyRegionRect>& dirty_regions,
    render::PresentReason reason,
    bool overlay_only_work,
    bool bypass_overlay_throttle,
    std::uint64_t event_sequence) {
  if (dirty_present_pipeline_ == nullptr) {
    return MakeInitFailure("render_runtime", "dirty present pipeline not initialized");
  }
  render::DirtyPresentEvent event{};
  event.event_sequence = event_sequence;
  event.reason = reason;
  event.monitor_target = render::DirtyMonitorTarget::kAllActiveMonitors;
  event.coord_space = render::DirtyRectCoordSpace::kDesktopPixels;
  event.force_full_frame = dirty_regions.empty();
  event.queue_overflow_recovery = false;
  event.bypass_overlay_throttle = bypass_overlay_throttle;
  event.dirty_rects.reserve(dirty_regions.size());
  for (const auto& rect : dirty_regions) {
    if (!rect.IsValid()) {
      continue;
    }
    event.dirty_rects.push_back(ToDirtyRectPx(rect));
  }
  if (event.dirty_rects.empty()) {
    event.force_full_frame = true;
  }

  render::DirtyPresentPipelineUpdate update{};
  const render::RenderStatus queue_status =
      dirty_present_pipeline_->QueueDirtyEvent(event, &update);
  if (queue_status != render::RenderStatus::kOk &&
      queue_status != render::RenderStatus::kNotFound) {
    return MakeInitFailure("render_runtime", "failed to queue dirty present event");
  }

  if (overlay_only_work) {
    if (overlay_throttle_.RecordOverlayInvalidation(event_sequence)) {
      const auto decision = overlay_throttle_.Evaluate(
          OverlayTimeFromMs(NowMs()), true, bypass_overlay_throttle);
      if (decision.throttled && decision.next_deadline.has_value()) {
        overlay_only_deadline_ms_ = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                decision.next_deadline->time_since_epoch())
                .count());
        telemetry_.overlay_throttled += 1U;
      } else {
        overlay_only_deadline_ms_ = 0U;
      }
    }
  }

  return PresentPendingIntents(
      overlay_only_work, bypass_overlay_throttle, NowMs(), event_sequence);
}

RuntimeGraphResult AppHost::PresentPendingIntents(
    bool overlay_only_work,
    bool bypass_overlay_throttle,
    std::uint64_t now_ms,
    std::uint64_t event_sequence) {
  if (dirty_present_pipeline_ == nullptr || swapchain_manager_ == nullptr) {
    return MakeInitFailure("render_runtime", "render presenter not initialized");
  }

  const auto decision = overlay_throttle_.Evaluate(
      OverlayTimeFromMs(now_ms), overlay_only_work, bypass_overlay_throttle);
  if (overlay_only_work && decision.throttled) {
    telemetry_.overlay_throttled += 1U;
    if (decision.next_deadline.has_value()) {
      overlay_only_deadline_ms_ = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              decision.next_deadline->time_since_epoch())
              .count());
    }
    return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
  }
  overlay_only_deadline_ms_ = 0U;

  std::vector<render::PresentIntent> intents;
  const render::RenderStatus build_status =
      dirty_present_pipeline_->BuildPendingPresentIntents(&intents);
  if (build_status != render::RenderStatus::kOk) {
    return MakeInitFailure("render_runtime", "failed to build pending present intents");
  }

  bool any_presented = false;
  for (auto& intent : intents) {
    render::PresentIntent consumed{};
    if (dirty_present_pipeline_->ConsumePresentIntent(intent.monitor, &consumed) !=
        render::RenderStatus::kOk) {
      continue;
    }

    telemetry_.present_attempts += 1U;
    render::PresentResult present_result{};
    const render::RenderStatus present_status =
        swapchain_manager_->Present(consumed, &present_result);
    if (present_status == render::RenderStatus::kDeviceLost) {
      (void)device_recovery_->MarkDeviceLost(consumed.monitor.adapter, event_sequence);
      (void)device_recovery_->RequestRecovery(consumed.monitor.adapter, event_sequence);
      render::DeviceRecoveryStepResult recovery_result{};
      (void)device_recovery_->RunRecoveryStep(
          consumed.monitor.adapter, event_sequence, &recovery_result);
      continue;
    }
    if (present_status == render::RenderStatus::kOk && present_result.presented) {
      telemetry_.present_success += 1U;
      any_presented = true;
    }
  }

  if (overlay_only_work && any_presented && decision.has_pending_overlay) {
    overlay_throttle_.OnPresented(
        OverlayTimeFromMs(now_ms),
        decision.coalesced_sequence == 0U ? event_sequence : decision.coalesced_sequence);
  }

  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

void AppHost::PollRuntimeFeeds(std::uint64_t now_ms) {
  PollSnapshotWatcher(now_ms);
  ExpireOverlayEvents(now_ms);
  DrainDeferredIpcCommands(now_ms);

  if (pipe_server_ != nullptr && ipc_started_) {
    const ipc::PipeServerError poll_error =
        pipe_server_->Poll(std::chrono::milliseconds(0));
    if (poll_error != ipc::PipeServerError::kNone &&
        poll_error != ipc::PipeServerError::kPlatformUnsupported) {
      notes_.push_back("ipc_runtime: Poll returned non-ok state");
    }
  }

  if (overlay_only_deadline_ms_ > 0U && now_ms >= overlay_only_deadline_ms_) {
    (void)PresentPendingIntents(true, false, now_ms, NextEventSequence());
  }
}

void AppHost::PollSnapshotWatcher(std::uint64_t now_ms) {
  if (snapshot_watcher_ == nullptr || !snapshot_watcher_started_) {
    return;
  }
  const auto next_generation = snapshot_watcher_->ClaimNextGeneration();
  if (!next_generation.has_value() || *next_generation <= last_snapshot_generation_) {
    return;
  }
  last_snapshot_generation_ = *next_generation;

  const std::string snapshot_path = config_.snapshot_watch_directory + "/" +
                                    config_.snapshot_filename_filter;
  const std::vector<std::uint8_t> bytes = ReadBinaryFile(snapshot_path);
  if (bytes.empty()) {
    notes_.push_back("ingest_runtime: snapshot generation observed but payload unavailable");
    return;
  }

  std::vector<overlay::DirtyRegionDelta> deltas;
  if (!ApplySnapshotFromBytes(bytes, now_ms, &deltas)) {
    notes_.push_back("ingest_runtime: snapshot parse rejected");
    return;
  }
  std::vector<overlay::DirtyRegionRect> dirty =
      overlay::DirtyRegionGenerator::GenerateForBatch(std::move(deltas));
  (void)DispatchDirtyPresentEvent(
      dirty,
      render::PresentReason::kInvalidation,
      true,
      false,
      NextEventSequence());
}

void AppHost::ExpireOverlayEvents(std::uint64_t now_ms) {
  if (overlay_event_store_ == nullptr) {
    return;
  }
  const auto expired = overlay_event_store_->Expire(EventStoreTimeFromMs(now_ms));
  if (expired.empty()) {
    return;
  }

  std::vector<overlay::DirtyRegionDelta> deltas;
  deltas.reserve(expired.size());
  for (const auto& item : expired) {
    overlay::DirtyRegionDelta delta{};
    delta.event_id = item.event_id;
    delta.event_sequence = item.event_sequence;
    delta.old_bounds = item.bounds;
    delta.new_bounds = std::nullopt;
    delta.padding_px = config_.overlay_padding_px;
    delta.force_redraw_if_unchanged = false;
    deltas.push_back(std::move(delta));
  }
  auto dirty = overlay::DirtyRegionGenerator::GenerateForBatch(std::move(deltas));
  (void)DispatchDirtyPresentEvent(
      dirty,
      render::PresentReason::kInvalidation,
      true,
      false,
      NextEventSequence());
}

void AppHost::DrainDeferredIpcCommands(std::uint64_t now_ms) {
  if (arbitration_engine_ == nullptr) {
    return;
  }
  auto deferred = arbitration_engine_->DrainDeferred(now_ms);
  if (deferred.empty()) {
    return;
  }
  for (const auto& command : deferred) {
    bool applied = false;
    switch (command.request.channel) {
      case input::ArbitrationChannel::kCameraRotate:
        applied = ApplyCameraRotateCommand(
            static_cast<float>(command.request.payload_fingerprint & 0xFFFFU) * 0.001F,
            static_cast<float>((command.request.payload_fingerprint >> 16U) & 0xFFFFU) *
                0.001F);
        break;
      case input::ArbitrationChannel::kCameraZoom:
        applied = ApplyCameraZoomCommand(
            static_cast<float>(command.request.payload_fingerprint & 0xFFFFU) * 0.001F);
        break;
      default:
        applied = true;
        break;
    }
    if (applied) {
      (void)DispatchDirtyPresentEvent(
          {},
          render::PresentReason::kInvalidation,
          false,
          true,
          NextEventSequence());
    }
  }
}

std::uint64_t AppHost::NextEventSequence() {
  runtime_event_sequence_ += 1U;
  telemetry_.event_sequence = runtime_event_sequence_;
  return runtime_event_sequence_;
}

bool AppHost::ApplySnapshotFromBytes(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t now_ms,
    std::vector<overlay::DirtyRegionDelta>* out_deltas) {
  if (overlay_event_store_ == nullptr || out_deltas == nullptr) {
    return false;
  }

  ingest::SnapshotParseContext context{};
  context.last_applied_sequence = last_snapshot_sequence_;
  const ingest::SnapshotParseResult parsed = ingest::ParseSnapshotV1(bytes, context);
  if (parsed.disposition != ingest::SnapshotDisposition::kApply) {
    return false;
  }

  out_deltas->clear();
  const auto now = EventStoreTimeFromMs(now_ms);
  if (parsed.header.snapshot_kind ==
      static_cast<std::uint8_t>(ingest::SnapshotKind::kFullState)) {
    const std::uint64_t clear_sequence =
        parsed.header.sequence == 0U ? 1U : parsed.header.sequence;
    (void)overlay_event_store_->ClearAll(clear_sequence);
    overlay_record_types_.clear();
  }

  for (const auto& record : parsed.records) {
    if (record.skipped_unknown_optional) {
      continue;
    }
    const auto* payload = bytes.data() + record.payload_offset_bytes;
    const auto& envelope = record.envelope;
    const auto record_type = static_cast<ingest::RecordType>(envelope.record_type);

    if (record_type == ingest::RecordType::kClearEvent) {
      const auto* clear_payload =
          reinterpret_cast<const ingest::ClearEventPayloadPrefixWire*>(payload);
      if (clear_payload->clear_scope ==
          static_cast<std::uint8_t>(ingest::ClearScope::kAll)) {
        (void)overlay_event_store_->ClearAll(parsed.header.sequence);
        overlay_record_types_.clear();
      } else if (clear_payload->clear_scope ==
                 static_cast<std::uint8_t>(ingest::ClearScope::kByEventId)) {
        const std::string event_id = OverlayEventIdFromU64(clear_payload->target_event_id);
        const auto previous = overlay_event_store_->Get(event_id);
        if (overlay_event_store_->Clear(event_id, parsed.header.sequence) ==
            overlay::EventStoreResult::kApplied) {
          overlay_record_types_.erase(event_id);
          overlay::DirtyRegionDelta delta{};
          delta.event_id = event_id;
          delta.event_sequence = parsed.header.sequence;
          if (previous.has_value()) {
            delta.old_bounds = previous->bounds;
          }
          delta.new_bounds = std::nullopt;
          delta.padding_px = config_.overlay_padding_px;
          out_deltas->push_back(std::move(delta));
        }
      } else if (clear_payload->clear_scope ==
                 static_cast<std::uint8_t>(ingest::ClearScope::kByType)) {
        const std::uint8_t target_type = clear_payload->target_type;
        std::vector<std::string> ids_to_clear;
        for (const auto& [id, type] : overlay_record_types_) {
          if (type == target_type) {
            ids_to_clear.push_back(id);
          }
        }
        for (const std::string& id : ids_to_clear) {
          const auto previous = overlay_event_store_->Get(id);
          if (overlay_event_store_->Clear(id, parsed.header.sequence) ==
              overlay::EventStoreResult::kApplied) {
            overlay_record_types_.erase(id);
            overlay::DirtyRegionDelta delta{};
            delta.event_id = id;
            delta.event_sequence = parsed.header.sequence;
            if (previous.has_value()) {
              delta.old_bounds = previous->bounds;
            }
            delta.new_bounds = std::nullopt;
            delta.padding_px = config_.overlay_padding_px;
            out_deltas->push_back(std::move(delta));
          }
        }
      }
      continue;
    }

    overlay::OverlayBounds bounds{};
    if (record_type == ingest::RecordType::kPointAlert) {
      const auto* point = reinterpret_cast<const ingest::PointAlertPayloadPrefixWire*>(payload);
      bounds = BoundsFromPoint(
          static_cast<std::int32_t>(point->x_px),
          static_cast<std::int32_t>(point->y_px),
          static_cast<std::int32_t>(point->size_px));
    } else if (record_type == ingest::RecordType::kAreaAlert) {
      const auto* area = reinterpret_cast<const ingest::AreaAlertPayloadPrefixWire*>(payload);
      bounds.left = static_cast<std::int32_t>(area->x_min_px);
      bounds.top = static_cast<std::int32_t>(area->y_min_px);
      bounds.right = static_cast<std::int32_t>(area->x_max_px);
      bounds.bottom = static_cast<std::int32_t>(area->y_max_px);
    } else if (record_type == ingest::RecordType::kScanRing) {
      const auto* ring = reinterpret_cast<const ingest::ScanRingPayloadPrefixWire*>(payload);
      bounds = BoundsFromRing(
          static_cast<std::int32_t>(ring->center_x_px),
          static_cast<std::int32_t>(ring->center_y_px),
          static_cast<std::int32_t>(ring->radius_px));
    } else if (record_type == ingest::RecordType::kScanArc) {
      const auto* arc = reinterpret_cast<const ingest::ScanArcPayloadPrefixWire*>(payload);
      bounds = BoundsFromRing(
          static_cast<std::int32_t>(arc->center_x_px),
          static_cast<std::int32_t>(arc->center_y_px),
          static_cast<std::int32_t>(arc->radius_px));
    } else {
      continue;
    }

    const std::string event_id = OverlayEventIdFromU64(envelope.event_id);
    const auto previous = overlay_event_store_->Get(event_id);
    const auto upsert_result = overlay_event_store_->Upsert(
        event_id,
        bounds,
        parsed.header.sequence,
        std::chrono::milliseconds(envelope.ttl_ms),
        now);
    if (upsert_result == overlay::EventStoreResult::kApplied) {
      overlay_record_types_[event_id] = envelope.record_type;
      overlay::DirtyRegionDelta delta{};
      delta.event_id = event_id;
      delta.event_sequence = parsed.header.sequence;
      if (previous.has_value()) {
        delta.old_bounds = previous->bounds;
      }
      delta.new_bounds = bounds;
      delta.padding_px = config_.overlay_padding_px;
      delta.force_redraw_if_unchanged = false;
      out_deltas->push_back(std::move(delta));
    }
  }

  last_snapshot_sequence_ = parsed.header.sequence;
  return true;
}

bool AppHost::ApplyIpcFrame(
    std::uint64_t connection_id,
    const ipc::PipeFrame& frame,
    std::uint64_t now_ms) {
  if (arbitration_engine_ == nullptr || pipe_server_ == nullptr) {
    return false;
  }
  if (frame.header.msg_type != ipc::PipeMessageType::kCommand ||
      frame.payload.size() < kCommandEnvelopeBytes) {
    return false;
  }

  const std::uint16_t command_id_raw = ipc::ReadU16Le(frame.payload, 0U);
  const auto command_id = static_cast<ipc::PipeCommandId>(command_id_raw);
  const input::ArbitrationChannel channel = ChannelForCommand(command_id);

  input::ArbitrationRequest request{};
  request.source = input::ArbitrationSource::kIpc;
  request.channel = channel;
  request.source_sequence = frame.header.sequence;
  request.monotonic_time_ms = now_ms;
  request.user_interaction_epoch = input_router_ != nullptr
                                       ? input_router_->snapshot().interaction_epoch
                                       : 0U;
  request.emergency = false;
  request.allow_deferred_coalescing = true;
  request.payload_fingerprint =
      (static_cast<std::uint64_t>(command_id_raw) << 32U) |
      static_cast<std::uint64_t>(frame.header.payload_crc32c);

  const input::ArbitrationDecision decision = arbitration_engine_->Evaluate(request);
  bool applied = false;
  bool rejected = false;
  if (decision.should_execute_now) {
    switch (command_id) {
      case ipc::PipeCommandId::kRotate: {
        if (frame.payload.size() >= (kCommandEnvelopeBytes + 12U)) {
          const float yaw = ipc::ReadF32Le(frame.payload, 8U);
          const float pitch = ipc::ReadF32Le(frame.payload, 12U);
          applied = ApplyCameraRotateCommand(yaw, pitch);
        }
        break;
      }
      case ipc::PipeCommandId::kZoom: {
        if (frame.payload.size() >= (kCommandEnvelopeBytes + 4U)) {
          applied = ApplyCameraZoomCommand(ipc::ReadF32Le(frame.payload, 8U));
        }
        break;
      }
      case ipc::PipeCommandId::kStyle:
      case ipc::PipeCommandId::kSpan:
      case ipc::PipeCommandId::kMirror:
      case ipc::PipeCommandId::kCameraMove:
        applied = true;
        break;
      default:
        rejected = true;
        break;
    }
  } else if (decision.disposition == input::ArbitrationDisposition::kRejected) {
    rejected = true;
  }

  ipc::PipeFrame outbound{};
  outbound.header.msg_type =
      rejected ? ipc::PipeMessageType::kError : ipc::PipeMessageType::kAck;
  outbound.header.sequence = frame.header.sequence;
  outbound.header.ack_sequence = frame.header.sequence;
  outbound.header.flags = 0U;
  outbound.payload.resize(24U, 0U);

  ipc::WriteU32Le(outbound.payload.data(), 0U, frame.header.sequence);
  ipc::WriteU16Le(
      outbound.payload.data(),
      4U,
      static_cast<std::uint16_t>(frame.header.msg_type));
  ipc::WriteU16Le(outbound.payload.data(), 6U, command_id_raw);

  if (rejected) {
    ipc::WriteU32Le(
        outbound.payload.data(),
        8U,
        static_cast<std::uint32_t>(ipc::PipeErrorCode::kMalformedPayload));
    ipc::WriteU32Le(outbound.payload.data(), 12U, 0U);
    ipc::WriteU32Le(outbound.payload.data(), 16U, 0U);
    ipc::WriteU8(
        outbound.payload.data(),
        20U,
        static_cast<std::uint8_t>(ipc::PipeErrorHandling::kContinue));
    telemetry_.ipc_error_count += 1U;
  } else {
    ipc::WriteU32Le(outbound.payload.data(), 8U, BuildAckResultFlags(decision));
    control_state_revision_ += applied ? 1U : 0U;
    ipc::WriteU32Le(outbound.payload.data(), 12U, control_state_revision_);
    ipc::WriteU32Le(
        outbound.payload.data(),
        16U,
        static_cast<std::uint32_t>(decision.deferred_count));
    ipc::WriteU32Le(outbound.payload.data(), 20U, 0U);
    telemetry_.ipc_ack_count += 1U;
  }

  (void)pipe_server_->QueueFrame(connection_id, outbound);
  if (applied) {
    (void)DispatchDirtyPresentEvent(
        {},
        render::PresentReason::kInvalidation,
        false,
        true,
        NextEventSequence());
  }
  return !rejected;
}

void AppHost::HandlePipeEvent(const ipc::PipeServerEvent& event) {
  const std::uint64_t now_ms = NowMs();
  switch (event.kind) {
    case ipc::PipeServerEventKind::kClientConnected:
      active_pipe_connection_id_ = event.connection_id;
      break;
    case ipc::PipeServerEventKind::kClientDisconnected:
      if (event.connection_id == active_pipe_connection_id_) {
        active_pipe_connection_id_ = 0U;
      }
      break;
    case ipc::PipeServerEventKind::kFrameReceived:
      if (event.frame.has_value()) {
        (void)ApplyIpcFrame(event.connection_id, *event.frame, now_ms);
      }
      break;
    case ipc::PipeServerEventKind::kProtocolError:
      telemetry_.ipc_error_count += 1U;
      break;
    default:
      break;
  }
}

bool AppHost::ApplyCameraRotateCommand(float delta_yaw_deg, float delta_pitch_deg) {
  scene::CameraTickInput tick{};
  tick.orbit.yaw_delta_rad = static_cast<double>(delta_yaw_deg) * 0.017453292519943295;
  tick.orbit.pitch_delta_rad = static_cast<double>(delta_pitch_deg) * 0.017453292519943295;
  camera_controller_.Tick(1.0 / 60.0, tick);
  return true;
}

bool AppHost::ApplyCameraZoomCommand(float delta) {
  scene::CameraTickInput tick{};
  tick.orbit.zoom_delta = static_cast<double>(delta);
  const auto state = camera_controller_.state();
  tick.orbit.has_cursor_ray = true;
  tick.orbit.cursor_ray_origin_ecef = state.position_ecef;
  const double cos_pitch = std::cos(state.pitch_rad);
  tick.orbit.cursor_ray_direction_ecef = scene::Normalize(scene::Vec3d{
      std::cos(state.yaw_rad) * cos_pitch,
      std::sin(state.yaw_rad) * cos_pitch,
      std::sin(state.pitch_rad),
  });
  camera_controller_.Tick(1.0 / 60.0, tick);
  return true;
}

std::uint32_t AppHost::BuildAckResultFlags(
    const input::ArbitrationDecision& decision) const {
  std::uint32_t flags = 0U;
  if (decision.disposition == input::ArbitrationDisposition::kApplied) {
    flags |= kAckAppliedFlag;
  }
  if (decision.disposition == input::ArbitrationDisposition::kCoalesced) {
    flags |= kAckCoalescedFlag;
  }
  if (decision.disposition == input::ArbitrationDisposition::kDeferred) {
    flags |= kAckDeferredFlag;
  }
  return flags;
}

input::ArbitrationChannel AppHost::ChannelForCommand(ipc::PipeCommandId command_id) {
  switch (command_id) {
    case ipc::PipeCommandId::kRotate:
      return input::ArbitrationChannel::kCameraRotate;
    case ipc::PipeCommandId::kZoom:
      return input::ArbitrationChannel::kCameraZoom;
    case ipc::PipeCommandId::kCameraMove:
      return input::ArbitrationChannel::kCameraTranslate;
    case ipc::PipeCommandId::kStyle:
      return input::ArbitrationChannel::kOverlayStyle;
    case ipc::PipeCommandId::kSpan:
    case ipc::PipeCommandId::kMirror:
      return input::ArbitrationChannel::kCameraMode;
  }
  return input::ArbitrationChannel::kSystemControl;
}

std::string AppHost::OverlayEventIdFromU64(std::uint64_t event_id) {
  return std::to_string(event_id);
}

std::string AppHost::SerializeRuntimeState(std::uint64_t now_ms) const {
  std::ostringstream out;
  const auto state = camera_controller_.state();
  out << "sequence=" << runtime_event_sequence_ << "\n";
  out << "saved_at_ms=" << now_ms << "\n";
  out << "camera_mode=" << static_cast<int>(state.mode) << "\n";
  out << "camera_pos=" << state.position_ecef.x << "," << state.position_ecef.y << ","
      << state.position_ecef.z << "\n";
  out << "camera_yaw_pitch=" << state.yaw_rad << "," << state.pitch_rad << "\n";
  out << "camera_orbit_target=" << state.orbit_target_ecef.x << ","
      << state.orbit_target_ecef.y << "," << state.orbit_target_ecef.z << "\n";
  out << "camera_orbit_radius=" << state.orbit_radius_m << "\n";
  return out.str();
}

bool AppHost::RestoreRuntimeState(const std::string& payload) {
  if (payload.empty()) {
    return false;
  }

  std::istringstream stream(payload);
  std::string line;
  scene::CameraState restored = camera_controller_.state();
  bool changed = false;
  while (std::getline(stream, line)) {
    const auto pos = line.find('=');
    if (pos == std::string::npos || pos == 0U) {
      continue;
    }
    const std::string key = line.substr(0U, pos);
    const std::string value = line.substr(pos + 1U);
    try {
      if (key == "camera_mode") {
        restored.mode = static_cast<scene::CameraMode>(std::stoi(value));
        changed = true;
      } else if (key == "camera_pos") {
        std::istringstream coords(value);
        std::string token;
        std::getline(coords, token, ',');
        restored.position_ecef.x = std::stod(token);
        std::getline(coords, token, ',');
        restored.position_ecef.y = std::stod(token);
        std::getline(coords, token, ',');
        restored.position_ecef.z = std::stod(token);
        changed = true;
      } else if (key == "camera_yaw_pitch") {
        std::istringstream coords(value);
        std::string token;
        std::getline(coords, token, ',');
        restored.yaw_rad = std::stod(token);
        std::getline(coords, token, ',');
        restored.pitch_rad = std::stod(token);
        changed = true;
      } else if (key == "camera_orbit_target") {
        std::istringstream coords(value);
        std::string token;
        std::getline(coords, token, ',');
        restored.orbit_target_ecef.x = std::stod(token);
        std::getline(coords, token, ',');
        restored.orbit_target_ecef.y = std::stod(token);
        std::getline(coords, token, ',');
        restored.orbit_target_ecef.z = std::stod(token);
        changed = true;
      } else if (key == "camera_orbit_radius") {
        restored.orbit_radius_m = std::stod(value);
        changed = true;
      }
    } catch (...) {
      return false;
    }
  }
  if (changed) {
    camera_controller_.SetState(restored);
  }
  return changed;
}

}  // namespace ntium::app
