# Render Invalidation API Contract v1

- **Ticket:** C0-003
- **Status:** Frozen for implementation
- **Owners:** A0 + A2 + A4
- **Primary consumers:** `src/ingest/*`, `src/overlay/*`, `src/input/*`, `src/render/*`
- **Language target:** C++17/Win32/D3D11

## 1) Scope

This document defines the API and behavioral contract for invalidation handoff from **ingest**, **overlay**, and **input** subsystems into the **render core**.

It freezes:

1. Thread ownership and allowed call sites.
2. Dirty-rectangle payload format and merge/fallback rules.
3. Per-monitor mapping semantics.
4. Overlay burst throttling (`<= 10 Hz` presents).
5. Event reason taxonomy and render invalidation state transitions.
6. Acceptance invariants required by downstream implementation/tests.

Normative terms: **MUST**, **MUST NOT**, **SHALL**, **SHOULD**.

---

## 2) Threading and ownership

## 2.1 Producer threads

The only cross-component producers for this API are:

- Ingest worker thread(s) (`ingest`)
- Overlay engine thread(s) (`overlay`)
- Input/arbitration thread(s) (`input`)

These threads MAY call the invalidation sink concurrently.

## 2.2 Consumer thread

- Render core owns a single consumer lane (`render thread`).
- All D3D11/DXGI calls MUST occur on the render thread.
- Producers MUST NOT call any render-device/swapchain API directly.

## 2.3 Allowed call sites

Allowed API entrypoint:

```cpp
// Thread-safe MPSC entrypoint.
// Non-blocking: bounded enqueue, no waiting on render thread.
virtual bool PostInvalidation(InvalidationEvent&& event) noexcept = 0;
```

Rules:

1. `PostInvalidation` MAY be called from ingest/overlay/input threads only.
2. `PostInvalidation` MUST be wait-free or lock-free from caller perspective (no blocking on GPU/Present).
3. If enqueue fails (queue full), implementation MUST set a sticky `force_full_frame_all_monitors` flag and return `false`; render thread then performs full-frame recovery present at next opportunity.
4. Ownership transfer is by move: on success, render core owns `event` memory.

## 2.4 Sequence ordering

- A global 64-bit `event_seq` MUST be assigned at enqueue boundary (`fetch_add(1)`).
- Render thread MUST process events in strictly increasing `event_seq`.
- Any producer-local sequence fields are informational only and MUST NOT override global order.

---

## 3) API surface (implementation-ready shape)

```cpp
namespace render {

enum class InvalidationSource : uint8_t {
  Ingest = 0,
  Overlay = 1,
  Input = 2,
  Runtime = 3  // internal/system events: topology/device/occlusion
};

enum class InvalidationReason : uint16_t {
  // Ingest-driven
  SnapshotFullStateApplied,
  SnapshotDeltaApplied,
  SnapshotParseRecovered,          // kept last good, request redraw if needed

  // Overlay-driven
  OverlayEventUpsert,
  OverlayEventRemoved,
  OverlayEventExpired,
  OverlayClearAll,
  OverlayStyleChanged,

  // Input-driven
  CameraTransformChanged,
  ViewportInteractionEnded,        // final settle/frame

  // Runtime/render lifecycle
  MonitorTopologyChanged,
  MonitorDpiChanged,
  SwapchainResized,
  DeviceRecovered,
  OcclusionCleared,

  // Control/fallback
  ForceFullFrame
};

enum class MonitorTarget : uint8_t {
  PrimaryOnly,
  AllActiveMonitors,
  ExplicitSet
};

struct DirtyRectPx {
  // Desktop or monitor-local depending on coord_space.
  // Inclusive-exclusive edges: [left, right), [top, bottom)
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;
};

enum class CoordSpace : uint8_t {
  DesktopPixels,    // virtual desktop physical pixel space
  MonitorLocalPixels
};

struct InvalidationEvent {
  uint64_t event_seq;                   // assigned by sink/router
  InvalidationSource source;
  InvalidationReason reason;
  MonitorTarget monitor_target;
  CoordSpace coord_space;
  uint64_t state_generation;            // source's authoritative state generation

  // Used when monitor_target == ExplicitSet.
  // Stable monitor identity is adapter_luid + target_id.
  std::vector<MonitorId> monitor_ids;

  // Empty rect list is valid only when force_full_frame == true
  // or reason implies full-frame (see section 5.3).
  std::vector<DirtyRectPx> dirty_rects;

  bool force_full_frame;
  bool bypass_overlay_throttle;         // true for ingest/input/runtime urgent reasons
};

class IRenderInvalidationSink {
public:
  virtual ~IRenderInvalidationSink() = default;
  virtual bool PostInvalidation(InvalidationEvent&& event) noexcept = 0;
};

}  // namespace render
```

Required constants:

```cpp
constexpr uint32_t kMaxDirtyRectsPerMonitor = 32;
constexpr double   kFullFrameAreaThreshold = 0.60; // >= 60% monitor coverage => full frame
constexpr auto     kOverlayBurstPresentMinInterval = 100ms; // <= 10 Hz
```

---

## 4) Dirty-rect contract

## 4.1 Rect validity

Each rect MUST satisfy:

- `left < right`
- `top < bottom`
- finite 32-bit integer coordinates

Invalid rects MUST be dropped before enqueue. If all rects are dropped and redraw is still required, producer MUST set `force_full_frame=true`.

## 4.2 Coordinate rules

1. Preferred producer format: `CoordSpace::DesktopPixels`.
2. `CoordSpace::MonitorLocalPixels` MAY be used only when `monitor_target == ExplicitSet`.
3. DIP/logical coordinates MUST be converted by producer before posting; render core only consumes physical pixel coordinates.

## 4.3 Coalescing responsibility split

- **Producer responsibility (ingest/overlay/input):**
  - Emit semantically correct dirty regions for changed content.
  - Optional local dedupe/coalesce is allowed.
  - MUST mark `force_full_frame=true` when semantic diff is unknown.

- **Render core responsibility (authoritative coalescing):**
  - Clip rects to target monitor bounds.
  - Split desktop-space rects by intersecting monitor bounds.
  - Merge overlapping/touching rects per monitor.
  - Enforce `kMaxDirtyRectsPerMonitor` cap.
  - Convert to full-frame when fallback conditions are hit.

## 4.4 Full-frame fallback conditions

Render core MUST promote to full-frame for affected monitor(s) when any is true:

1. `event.force_full_frame == true`.
2. `event.reason` is one of:
   - `SnapshotFullStateApplied`
   - `OverlayClearAll`
   - `MonitorTopologyChanged`
   - `MonitorDpiChanged`
   - `SwapchainResized`
   - `DeviceRecovered`
   - `ForceFullFrame`
3. Post-clip rect count exceeds `kMaxDirtyRectsPerMonitor`.
4. Union dirty area / monitor area `>= kFullFrameAreaThreshold`.
5. Queue-overflow recovery flag is set.

---

## 5) Per-monitor behavior and mapping

## 5.1 Stable monitor identity

Monitor identity MUST be stable across reorder/hotplug:

- `MonitorId = {adapter_luid, target_id}` (or equivalent immutable pair).
- HMONITOR index ordering MUST NOT be used as persistent identity.

## 5.2 Mapping rules

For each event:

1. Resolve target monitor set:
   - `PrimaryOnly` -> current primary monitor only.
   - `AllActiveMonitors` -> every active monitor with swapchain.
   - `ExplicitSet` -> provided `monitor_ids` intersected with currently active monitors.
2. If `coord_space == DesktopPixels`, intersect each rect with each monitor desktop bounds and transform to monitor-local by subtracting monitor origin.
3. If a rect spans multiple monitors, it MUST be split into per-monitor fragments.
4. If no active monitor remains after mapping, event is consumed with no present.

## 5.3 Topology race handling

If monitor topology changes while events are queued:

1. Inject internal `MonitorTopologyChanged` with new `event_seq`.
2. Drop stale per-monitor rect caches keyed to removed monitor IDs.
3. Force full-frame on all currently active monitors once.
4. Resume normal dirty-rect flow after the topology sync frame.

---

## 6) Overlay burst throttling contract

Goal: under high-rate overlay updates, present no faster than **10 Hz** while always converging to the latest state.

Rules:

1. For **overlay-only pending work** (all pending reasons from `InvalidationSource::Overlay` and non-urgent):
   - Presents MUST NOT occur more frequently than `kOverlayBurstPresentMinInterval` (`100 ms`).
2. Ingest/input/runtime urgent work (`bypass_overlay_throttle=true`) MAY present immediately and piggyback latest overlay dirt.
3. During throttle window, additional overlay events MUST be coalesced (latest-state wins).
4. Throttle MUST use one-shot timer or deadline scheduling; no spin/poll loops.
5. Once deadline expires, exactly one present is issued if dirty state remains.

Compliance metric:

- In burst tests (20 Hz, 50 Hz overlay event streams), measured present frequency for overlay-only periods MUST be `<= 10 Hz`.

---

## 7) Invalidation state machine

Render invalidation FSM (per render core instance):

```text
Idle
  | (event enqueued)
  v
DirtyQueued
  | (eligible to present now)
  v
PresentScheduled
  | (render thread begins frame)
  v
Presenting
  |--(DXGI_STATUS_OCCLUDED)--> OccludedHold
  |--(present success, no pending dirt)--> Idle
  |--(present success, pending dirt)--> DirtyQueued

OccludedHold
  | (new dirt arrives) stays OccludedHold, accumulate latest dirt
  | (OcclusionCleared / visibility restored)
  v
DirtyQueued
```

State transition requirements:

1. `Idle -> DirtyQueued` only on enqueue of new effective dirty work.
2. `DirtyQueued -> PresentScheduled` only when not blocked by throttle/occlusion/device-loss recovery.
3. No direct `Idle -> Presenting` transition.
4. `Presenting` MUST atomically clear only the dirt actually presented; newly enqueued dirt remains pending for next cycle.

---

## 8) Acceptance invariants

These invariants are mandatory and testable.

## 8.1 No idle loop

- System MUST be event-driven.
- When no invalidation work is pending, render thread MUST block on synchronization primitive (queue/event/timer wait).
- Continuous `Present` without new invalidation is forbidden.

## 8.2 No redundant presents

- A present MUST NOT be issued when there is no effective dirty region and no forced full-frame reason.
- Duplicate invalidation events that coalesce to empty net change MUST NOT trigger extra presents.

## 8.3 Deterministic ordering

- Processing order is strictly by global `event_seq`.
- Given identical event stream and topology timeline, emitted present decisions (dirty rect sets/full-frame flags per monitor) MUST be identical run-to-run.

## 8.4 Bounded work

- Per-monitor dirty rect count after coalescing is bounded by `kMaxDirtyRectsPerMonitor` or converted to full-frame.
- Queue overflow degrades to deterministic full-frame recovery, never unbounded growth.

---

## 9) Contract examples

## 9.1 Single-monitor small overlay update

Input:

- Monitor: `M0` bounds `1920x1080`
- Event: `OverlayEventUpsert`, one rect `(100,100)-(220,180)`, desktop pixels

Expected:

- Mapped to `M0` local same coordinates
- Dirty set contains one rect
- One `Present1` with one dirty rect (unless throttle delays overlay-only burst)

## 9.2 Multi-monitor spanning rect

Input:

- `M0`: desktop `(0,0)-(1920,1080)`
- `M1`: desktop `(1920,0)-(3840,1080)`
- Event rect `(1800,200)-(2040,420)`

Expected:

- Split into:
  - `M0 local`: `(1800,200)-(1920,420)`
  - `M1 local`: `(0,200)-(120,420)`
- Present decisions evaluated independently per monitor.

## 9.3 Full-frame fallback

Input:

- Event reason `OverlayClearAll`, empty rect list

Expected:

- `force_full_frame` for all targeted monitors
- Present uses full monitor bounds even without explicit rects

---

## 10) Non-goals (for this contract version)

- No definition of scene graph internals or shader invalidation granularity.
- No wire/protocol serialization format (covered by C0-001/C0-002).
- No alternative graphics backends beyond D3D11/DXGI in v1.

---

## 11) Compatibility policy

- Additive enum values are allowed only at end of enum with backward-safe defaults.
- Changing constants in section 3 requires contract version bump (`v2`) and perf re-baseline.
- Removing or reinterpreting reasons/fields is a breaking change.
