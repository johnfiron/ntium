# Render Foundation Lifecycle and Recovery Model

- **Tickets:** R1-201, R1-202
- **Status:** Initial scaffold for render-core implementation
- **Applies to:** `src/render/RenderDeviceManager.*`, `src/render/SwapchainManager.*`

## 1) Scope and intent

This foundation defines two core ownership boundaries:

1. **Per-adapter device management**
   - `IRenderDeviceManager` owns one logical device state keyed by `AdapterIdentity`.
2. **Per-monitor swapchain management**
   - `ISwapchainManager` owns one swapchain state keyed by `MonitorIdentity`.

Both managers are intentionally **event-driven**: they are consumed when topology, invalidation, occlusion, resize, or device-lost events occur. They are not designed for free-running polling or continuous present loops.

---

## 2) Identity model

Stable keys are required for deterministic behavior through hotplug/reorder:

- `AdapterIdentity = {luid_low_part, luid_high_part, adapter_key}`
- `MonitorIdentity = {adapter, target_id, monitor_key}`

`target_id` and adapter LUID pair are the authoritative identity axis. String keys are supplemental for diagnostics and tests.

---

## 3) Render device lifecycle (per adapter)

Primary API:

- `EnsureDeviceForAdapter(adapter)`
- `MarkDeviceLost(adapter, event_sequence)`
- `RecoverDevice(adapter, event_sequence)`
- `RemoveAdapter(adapter)`

Lifecycle states:

1. **Absent**
   - No adapter entry.
2. **Ready**
   - Adapter has a valid state and is eligible for swapchain operations.
3. **DeviceLost**
   - Runtime event marks adapter unavailable for present.
4. **Recovering**
   - Explicit recovery event attempts recreation.
5. **Ready (next generation)**
   - Recovery succeeded; generation increments and callback emits.

Recovery requirements:

- Recovery is adapter-scoped.
- Success increments generation monotically.
- Recovery should trigger dependent swapchain refresh work for monitors on the adapter.

---

## 4) Swapchain lifecycle (per monitor)

Primary API:

- `EnsureSwapchainForMonitor(monitor, descriptor, event_sequence)`
- `ResizeSwapchain(monitor, descriptor, event_sequence)`
- `Present(intent, out_result)`
- `MarkOccluded(monitor, is_occluded, event_sequence)`
- `RemoveMonitor(monitor)`

Lifecycle states:

1. **Absent**
   - No swapchain entry for monitor.
2. **Ready**
   - Swapchain exists and accepts event-driven presents.
3. **OccludedHold**
   - Present attempts are skipped/held until visible signal.
4. **RecreateRequired**
   - Resize/device loss or runtime status requires recreation.
5. **Ready (next generation)**
   - Recreated and resumed.

Present contract:

- Present occurs only in response to invalidation/runtime intent (`PresentIntent`).
- No "while(true) Present" loop is permitted by this model.
- Occluded monitors may absorb intent without presenting until visibility is restored.

---

## 5) Platform portability

- On `_WIN32`, implementations are intended to bind to D3D11 + DXGI objects.
- On non-Windows targets, managers return `RenderStatus::kUnsupportedPlatform` from creation/present paths while still preserving API shape and testability.

This keeps the repository portable and allows Linux/macOS CI to compile/type-check the render contracts.

---

## 6) Failure and recovery behavior

Expected deterministic handling:

1. Device-lost event marks adapter lost.
2. Subsequent swapchain ensure/present for that adapter reports `kDeviceLost`.
3. Recovery event attempts adapter recreation.
4. On recovery success:
   - Adapter generation increments.
   - swapchains tied to that adapter are marked `requires_recreate`.
5. Next event-driven render pass recreates swapchains and resumes normal presents.

If recreation fails repeatedly, managers stay in explicit degraded states (`kDeviceLost` or `kInternalError`) instead of spinning.

---

## 7) Relationship to invalidation contract

This scaffold intentionally aligns with `docs/contracts/render_invalidation_api.md`:

- stable monitor identity
- event sequence threading through lifecycle methods
- no idle-loop rendering
- deterministic recovery path after topology/device events

Future tickets should integrate these managers with the invalidation queue and scheduler so that present eligibility is decided by event state rather than a frame clock.
