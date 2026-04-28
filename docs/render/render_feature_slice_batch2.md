# Render Feature Slice — Batch 2 (R1-203/R1-204/R1-205)

- **Tickets:** R1-203, R1-204, R1-205
- **Status:** Foundation-grade implementation complete
- **Applies to:**
  - `src/render/DirtyPresentPipeline.*`
  - `src/render/DirtyRectCoalescer.*`
  - `src/render/DeviceRecovery.*`

## 1) Scope

Batch 2 adds deterministic, event-driven building blocks for:

1. Dirty present pipeline structures and APIs.
2. Dirty rectangle mapping/coalescing with full-frame fallback policy.
3. Device lost recovery state machine hooks for adapter and swapchain recovery.

This slice is intentionally scheduler-agnostic and has **no free-running render loop**. Host runtime is expected to call these APIs when explicit invalidation/runtime events occur.

---

## 2) Dirty present pipeline (`DirtyPresentPipeline`)

Primary responsibilities:

- Maintain active monitor layout snapshot.
- Accept dirty-present events.
- Map and coalesce dirty rects per monitor (via `IDirtyRectCoalescer`).
- Accumulate pending per-monitor present intents.
- Expose deterministic drain/consume APIs.

Primary API:

- `SetActiveMonitors(active_monitors, event_sequence)`
- `QueueDirtyEvent(event, out_update)`
- `BuildPendingPresentIntents(out_intents)`
- `ConsumePresentIntent(monitor, out_intent)`
- `HasPendingWork()`
- `ListPendingMonitors()`

Determinism guarantees:

- Monitor processing order is stable by adapter LUID + target id (+ key tie-breakers).
- Pending state is per monitor and overwritten/merged deterministically.
- Topology updates remove stale pending work for removed monitors.

Event-driven semantics:

- No implicit scheduling loop exists in pipeline internals.
- Runtime decides when to queue events and when to consume intents.

---

## 3) Dirty rect coalescer (`DirtyRectCoalescer`)

### 3.1 Per-monitor dirty mapping interface

Input:

- `DirtyPresentEvent`
  - monitor target (`primary`, `all`, or explicit set)
  - coord space (`desktop` or `monitor-local`)
  - reason / force-full-frame / queue-overflow flags
  - dirty rect list
- `std::vector<MonitorLayout>` active monitors

Output:

- `DirtyRectCoalescerResult`
  - `per_monitor`: local rects and fallback metadata for each targeted monitor
  - `batch_fallback_reason`
  - `has_effective_work`

Mapping behavior:

- Desktop-space rects are intersected with each target monitor desktop bounds.
- Spanning desktop rects are split into per-monitor fragments.
- Results are translated into monitor-local coordinates.
- Monitor-local coord input is accepted for explicit monitor targeting.

### 3.2 Coalescing and thresholds

Implemented policy:

1. Clip/mapping to monitor-local.
2. Merge overlap/touching rects (configurable touching-edge behavior).
3. Full-frame fallback when:
   - explicit forced full-frame intent
   - queue overflow recovery
   - reason requires full-frame (`TopologyChanged`, `DeviceRecovery`)
   - rect count exceeds `max_dirty_rects_per_monitor` (default `32`)
   - dirty coverage ratio exceeds `full_frame_area_threshold` (default `0.60`)
   - dirty set becomes invalid/empty for a targeted monitor
4. Full-frame rect is emitted as monitor bounds `(0,0)-(width,height)`.

Fallback reasons are explicit through `DirtyFullFrameFallbackReason` for observability and deterministic behavior.

---

## 4) Device lost recovery FSM (`DeviceRecovery`)

### 4.1 States

- `Ready`
- `DeviceLost`
- `RecoveryScheduled`
- `RecoveringDevice`
- `RebindingSwapchains`
- `Failed`

### 4.2 Hook model

Runtime injects behavior through `DeviceRecoveryHooks`:

- `recover_device(adapter, event_sequence)`
- `rebind_swapchains(adapter, event_sequence)`
- optional `on_recovery_succeeded(...)`

This keeps the FSM portable and testable while allowing platform-specific work (D3D11/DXGI recreate/rebind) to plug in externally.

### 4.3 Deterministic transitions

`RunRecoveryStep(...)` performs one deterministic attempt:

1. `DeviceLost` -> `RecoveryScheduled` (if needed)
2. `RecoveryScheduled` -> `RecoveringDevice`
3. Hook `recover_device`
4. On success: `RecoveringDevice` -> `RebindingSwapchains`
5. Hook `rebind_swapchains`
6. On success: `RebindingSwapchains` -> `Ready` and increment recovery generation

Failure path:

- recover/rebind failure increments retry count
- if retries remain: state returns to `RecoveryScheduled`
- if retry budget exceeded: state becomes `Failed` with reason `RetryBudgetExhausted`

Transition callback support is included for deterministic telemetry/integration.

---

## 5) Linux/non-Windows portability

- Batch 2 files are platform-neutral C++17 and compile on Linux.
- Platform-specific D3D11/DXGI operations remain behind existing manager hooks and `_WIN32` guarded implementation paths.
- This allows CI and non-Windows development to validate API shape and state logic now, while preserving Windows backend integration points.

---

## 6) Integration notes for next slices

1. Connect `DirtyPresentPipeline::BuildPendingPresentIntents` to swapchain present scheduling.
2. Feed device lost events to `MarkDeviceLost` + `RequestRecovery`.
3. Trigger `RunRecoveryStep` only from runtime events/timers (still no render loop).
4. Bind recovery hooks to:
   - `IRenderDeviceManager::RecoverDevice`
   - swapchain recreate/mark-recreate path per adapter monitors
5. Add focused unit coverage for:
   - multi-monitor desktop rect splits
   - threshold-triggered full-frame fallback
   - recovery retry/failed transitions and callback ordering
