# Desktop Host Foundation (H1-101, H1-103)

## Scope

This batch establishes a compilable Win32 desktop-host foundation with explicit
cross-platform stubs:

- `src/platform/win32/DesktopHost.h/.cpp`
- `src/platform/win32/ShellDesktopLocator.h/.cpp`
- `src/platform/win32/ShellEventRouter.h/.cpp`

The implementation is intentionally skeletal, but exposes concrete APIs for
attach/rebind orchestration and lifecycle event routing.

## Design Summary

### 1) DefView-relative attach strategy (no WorkerW dependency)

`ShellDesktopLocator` resolves a `DesktopAttachAnchor` by:

1. Looking for `SHELLDLL_DefView` under `Progman`.
2. Falling back to top-level shell window enumeration when needed.
3. Setting the attach target to **DefView's actual parent window**.

`WorkerW` is only observed for telemetry/diagnostics in the returned anchor:

- `workerw_observed`
- `workerw_window`

Control flow does **not** require WorkerW presence. Attach success is based on
`DefView` + parent availability only.

### 2) DesktopHost orchestration

`DesktopHost` is the coordinator for initialization, attachment, and lifecycle
rebind behavior:

- `Initialize()` wires `ShellEventRouter` callback and performs initial attach.
- `EnsureAttached(reason)` refreshes anchor state from `ShellDesktopLocator`.
- `DispatchWindowMessage(...)` routes incoming messages through
  `ShellEventRouter`.
- Lifecycle entry points are explicit/testable:
  - `OnTaskbarCreated()`
  - `OnDisplayChange(...)`
  - `OnSessionChange(...)`
  - `OnPowerBroadcast(...)`

Current behavior rebinding rules:

- Taskbar created: always rebind.
- Display change: rebind.
- Session events: rebind for connect/logon/unlock paths.
- Power events: rebind for resume events.

### 3) Shell event routing surface

`ShellEventRouter` provides both message-driven and direct entry points.

Win32 path:

- Registers `TaskbarCreated` via `RegisterWindowMessageA`.
- Parses and dispatches:
  - `WM_DISPLAYCHANGE`
  - `WM_WTSSESSION_CHANGE`
  - `WM_POWERBROADCAST`

Cross-platform path:

- Initializes deterministically with safe placeholders.
- Accepts direct entry-point calls for tests.
- Returns graceful `false` for unsupported runtime message dispatch.

## API Notes

- `DesktopHostAttachSnapshot` captures latest attach results, including optional
  WorkerW observation.
- `DesktopHost::AttachStrategySummary()` returns a stable strategy description
  for logs/diagnostics.
- `ShellEventRouter` callback model centralizes lifecycle handling without
  forcing HWND ownership into the host class.

## Test Plan

### Build/compile sanity

1. Compile all new `.cpp` units on non-Windows toolchains to verify
   preprocessor guards and stubs.
2. Compile on Windows with Win32 headers/libs available.

### Functional matrix (manual/integration)

1. **Cold start attach**
   - Expected: `attached=true` when DefView parent is found.
2. **Explorer restart (`TaskbarCreated`)**
   - Expected: deterministic rebind path via `OnTaskbarCreated`.
3. **Display change (`WM_DISPLAYCHANGE`)**
   - Expected: router invokes `OnDisplayChange`, host rebinds once.
4. **Session transitions (`WM_WTSSESSION_CHANGE`)**
   - Validate lock/unlock/logon/connect patterns trigger intended rebinds.
5. **Power resume (`WM_POWERBROADCAST`)**
   - Expected: resume events trigger rebind; unrelated power events do not.
6. **Missing DefView retry behavior**
   - Expected: `EnsureAttached` returns false, later retries can succeed without
     requiring WorkerW.

## Out-of-scope for this batch

- Actual child host window creation and placement.
- Icon visibility ownership controller (`H1-102`).
- ETW instrumentation and persistence around lifecycle decisions.
