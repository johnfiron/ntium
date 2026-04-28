# Input Feature Slice — Batch 2 (U1-503 / U1-504)

This feature slice introduces the input routing and arbitration core for the
Input epic:

- `src/input/InputRouter.h/.cpp`
- `src/input/ArbitrationEngine.h/.cpp`

The implementation is platform-agnostic by default and includes
Windows-oriented hook comments for integration points (focus/capture and system
cancel paths).

## 1) Input router model (`InputRouter`)

`InputRouter` defines deterministic state transitions for:

- mode: `passive` / `active`
- focus: `unfocused` / `focused`
- capture: `none` / `pointer_captured`
- interaction: `false` / `true` (user manipulation in progress)

### 1.1 State surface

`InputRouterSnapshot` is authoritative and includes:

- mode/focus/capture state
- `interaction_active`
- `interaction_epoch` (monotonic user-interaction generation)

`InputRouterEvent` emits before/after snapshots plus:

- transition reason (`focus lost`, `capture released`, `system cancel`, etc.)
- `should_flush_transient_input` flag for key/button cleanup edge-cases

### 1.2 Transition API

Core API:

- `SetMode(...)`
- `OnFocusChanged(...)`
- `AcquirePointerCapture(...)`
- `ReleasePointerCapture(...)`
- `BeginInteraction(...)`
- `EndInteraction(...)`
- `CancelInteractionForSystem(...)`

Routing gates:

- `CanRoutePointerInput()`
- `CanRouteKeyboardInput()`
- `ShouldHoldUserLease()`

### 1.3 Focus/capture behavior

Deterministic behavior under focus/capture transitions:

1. Entering active mode is suppressed if focus requirements are not met.
2. Focus loss emits `kFocusChanged` and can auto-passivate.
3. Passivation clears capture and active interaction.
4. Capture release clears in-flight interaction and requests transient-input
   flush.
5. System cancel path always clears capture/interaction safely.

This supports U1-503 edge-cases (`drag + alt-tab`, focus loss, key/button
release clean-up).

### 1.4 Windows integration hooks

The core logic remains platform-agnostic, while comments indicate intended
Win32 integration points:

- Call `AcquirePointerCapture()` after successful `SetCapture(hwnd)`.
- Call `ReleasePointerCapture()` on `ReleaseCapture()` or
  `WM_CAPTURECHANGED`.
- Call `OnFocusChanged(false, ...)` on `WM_KILLFOCUS`.
- Call `CancelInteractionForSystem(...)` on cancellation paths such as
  task-switch interruptions.

## 2) Arbitration model (`ArbitrationEngine`)

`ArbitrationEngine` arbitrates user-input vs IPC commands with a lease/deferred
coalescing model that prevents camera jitter under contention.

### 2.1 Source/channel model

Sources:

- `kUserInput`
- `kIpc`

Channels:

- camera channels: `translate`, `rotate`, `zoom`, `mode`
- non-lease channels: `overlay_style`, `system_control`

Only camera channels are lease-governed.

### 2.2 User lease model

`AcquireOrRenewUserLease(interaction_epoch, now_ms)` and
`ReleaseUserLease(interaction_epoch, now_ms)` control a lease window:

- lease is keyed to `interaction_epoch`
- lease expiry is time-based (`user_lease_duration_ms`)
- stale release calls (epoch mismatch) are ignored deterministically

`CancelUserLeaseForSystem(now_ms)` force-releases lease for focus/capture/system
interruptions.

### 2.3 Deferred IPC coalescing

When lease blocks IPC camera commands:

- if `allow_deferred_coalescing=false`: reject deterministically
- if `allow_deferred_coalescing=true`: queue one deferred slot per channel
- subsequent deferred commands coalesce to latest command in that channel
  (replace-in-slot behavior)

Each deferred slot tracks:

- original request
- deferred timestamp
- coalesce count

Deferred drain is deterministic channel order:

1. `system_control`
2. `camera_mode`
3. `camera_translate`
4. `camera_rotate`
5. `camera_zoom`
6. `overlay_style`

### 2.4 Deterministic conflict resolution rules

The decision table is stable and explicit:

1. Invalid request (`source_sequence == 0`) -> `rejected/invalid`.
2. Emergency request (`emergency=true`) -> immediate apply, lease preempted,
   deferred queue cleared.
3. User request on lease-governed channel -> apply + acquire/renew lease.
4. IPC request on non-lease channel -> apply.
5. IPC request on lease-governed channel with no active lease -> apply.
6. IPC request on lease-governed channel with active lease:
   - defer/coalesce if allowed
   - reject if non-coalescible

`ArbitrationDecision` exposes:

- disposition (`applied`, `deferred`, `coalesced`, `rejected`)
- reject reason
- deterministic decision sequence
- held-by-lease flag
- deferred queue depth

### 2.5 Event telemetry hooks

Event callbacks surface state-machine transitions:

- lease acquired/renewed/released/expired
- deferred queued/coalesced/drained
- emergency preemption

This provides deterministic observability for contention tests from U1-504.
