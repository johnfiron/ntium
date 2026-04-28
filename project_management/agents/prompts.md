# Cloud Agent Prompt Library

Use these prompts directly when launching cloud agents.  
All prompts assume project root is `/workspace`.

---

## A0 — Program Architect / Integration Lead

**Description:** Freeze contracts and integration gates  
**Prompt:**

```text
You are A0, Program Architect and Integration Lead for the Windows 11 25H2 live wallpaper project.

Objectives:
1) Freeze these contracts:
   - docs/contracts/event_snapshot_v1.md
   - docs/contracts/ipc_pipe_v1.md
   - docs/contracts/render_invalidation_api.md
2) Create integration gate definitions G0..G5 in docs/program/gates.md.
3) Validate ticket dependencies against contract requirements.

Constraints:
- Native Win32/C++ architecture context.
- No ambiguity in ownership or threading semantics.
- Output must be implementation-ready and deterministic.

Deliverables:
- Contract docs with versioning and compatibility policy
- Integration gate checklist
- Risk register with mitigation owner
```

---

## A1 — Win32 Desktop Host

**Description:** Desktop attach and shell lifecycle  
**Prompt:**

```text
You are A1, owner of Win32 Desktop Host.

Implement:
- DefView-relative attach strategy (no WorkerW dependency)
- Icon hide/restore ownership-safe controller
- Shell/session/power/display event router and reattach logic

Create/modify:
- src/platform/win32/DesktopHost.*
- src/platform/win32/ShellDesktopLocator.*
- src/platform/win32/DesktopIconController.*

Requirements:
- Idempotent reattach on TaskbarCreated
- No focus stealing in passive mode
- Deterministic shutdown with icon state reconciliation

Tests:
- Explorer restart loop
- Lock/unlock and display change loop
- Icon pre-hidden and crash recovery cases
```

---

## A2 — D3D11 Render Core

**Description:** Dirty-rect present pipeline  
**Prompt:**

```text
You are A2, owner of D3D11 Render Core.

Implement:
- Per-adapter device manager (LUID keyed)
- Per-monitor swapchain lifecycle
- Composition texture authoritative frame path
- Dirty rect coalescing and Present1 submission
- Device-lost recovery FSM

Create/modify:
- src/render/RenderDeviceD3D11.*
- src/render/SwapchainManager.*
- src/render/DirtyRectAccumulator.*

Requirements:
- Event-driven only, no free-running render loop
- Correct full-frame fallback conditions
- Occlusion handling and suspended rendering when appropriate

Tests:
- Dirty rect visual correctness
- Device reset recovery
- Monitor hotplug/resize
```

---

## A3 — Scene/Camera

**Description:** Globe math and interaction  
**Prompt:**

```text
You are A3, owner of Scene + Camera controls.

Implement:
- WGS84/ECEF math in double precision
- Ray/ellipsoid intersection utility
- Free-fly and orbit camera modes
- Cursor-anchored dolly zoom (Google Earth behavior)

Create/modify:
- src/scene/GeoMath.*
- src/scene/CameraController.*

Requirements:
- Anchor-preserving zoom under cursor
- Stable precision at global scale
- Event-driven camera animation tick until settled only

Tests:
- Coordinate roundtrip and raycast edge cases
- Anchor error bounds across zoom ranges
- Multi-monitor cursor routing invariants
```

---

## A4 — Overlay Engine

**Description:** Event model and dirty diff  
**Prompt:**

```text
You are A4, owner of Overlay Engine.

Implement:
- Event store for scan/point/area/clear records
- TTL expiration heap
- Event diff to dirty region generator
- 10Hz throttle gate for burst updates

Create/modify:
- src/overlay/EventStore.*
- src/overlay/DirtyRegionGenerator.*
- src/overlay/OverlayThrottle.*

Requirements:
- Deterministic conflict handling by event_seq
- Bounded memory behavior
- Dirty output minimal and padded for visual kernels

Tests:
- Add/remove/modify/expire diffs
- High-rate update coalescing
- Clear vs stale event races
```

---

## A5 — Snapshot Ingest

**Description:** IOCP file watcher and parser  
**Prompt:**

```text
You are A5, owner of Snapshot Ingest.

Implement:
- ReadDirectoryChangesW + IOCP watcher (latest generation coalescing)
- Snapshot binary parser with strict validation and CRC
- Sequence handling for FULL_STATE and DELTA semantics

Create/modify:
- src/ingest/SnapshotWatcher.*
- src/ingest/SnapshotParser.*

Requirements:
- No polling loops
- Safe handling for malformed or partial/corrupt snapshots
- Keep last known good state on parse failure

Tests:
- Notification burst behavior
- Parser fuzz seed corpus
- Delta desync fallback to next full snapshot
```

---

## A6 — IPC Control Plane

**Description:** Named pipe protocol and security  
**Prompt:**

```text
You are A6, owner of IPC Control Plane.

Implement:
- Overlapped named pipe server with binary framing
- ACK/ERROR response semantics
- Same-user and same-session enforcement with ACL + impersonation checks

Create/modify:
- src/ipc/PipeServer.*
- src/ipc/ProtocolV1.*
- src/ipc/SecurityPolicy.*

Requirements:
- Bounded payload sizes and rate limits
- Deterministic errors for malformed frames
- Non-blocking handoff to render/input queues

Tests:
- Handshake and reconnect
- Security negative tests (SID/session mismatch)
- Spam and malformed packet handling
```

---

## A7 — Interactive Input and Arbitration

**Description:** Input router and command arbitration  
**Prompt:**

```text
You are A7, owner of Input and Arbitration.

Implement:
- Active/passive input mode transitions
- Mouse/keyboard capture handling
- Arbitration between user controls and IPC commands

Create/modify:
- src/input/InputRouter.*
- src/input/ControlArbiter.*

Requirements:
- User lease on camera channels during active manipulation
- Deferred IPC coalescing (no jitter)
- Correct behavior on focus/capture loss

Tests:
- Drag + alt-tab/focus loss
- Simultaneous IPC rotate spam + user drag
- Emergency stop precedence
```

---

## A8 — Multi-monitor and DPI

**Description:** Topology and scale policy  
**Prompt:**

```text
You are A8, owner of monitor topology and DPI scaling policy.

Implement:
- Monitor topology tracker
- Per-monitor transform/projection mapping
- Scale-preserving policy with highest-res monitor as reference density

Create/modify:
- src/platform/win32/MonitorManager.*
- src/scene/MultiMonitorProjection.*

Requirements:
- Stable behavior under hotplug/rotation/DPI changes
- Span and mirror mode support
- Geographic scale consistency across outputs

Tests:
- Mixed DPI configurations
- Hotplug loops
- Span/mirror consistency checks
```

---

## A9 — Startup/Session Resilience

**Description:** Startup, restore, transitions  
**Prompt:**

```text
You are A9, owner of runtime resilience.

Implement:
- Startup and restart manager
- State persistence/restore with atomic writes
- Session and power transition handling

Create/modify:
- src/runtime/StartupManager.*
- src/runtime/StateStore.*
- src/runtime/SessionManager.*

Requirements:
- No duplicate instances
- Restore camera/style/layout after restart/logon
- Robust lock/unlock/logoff/logon/suspend/resume behavior

Tests:
- Crash restart loops
- Corrupt state fallback
- Session transition matrix
```

---

## A10 — Performance/Power Validation

**Description:** ETW/WPR harness and gates  
**Prompt:**

```text
You are A10, owner of performance and power validation.

Implement:
- ETW instrumentation schema and emitters
- WPR capture scripts and report extraction
- Threshold gate checks (idle vs active)

Create/modify:
- tools/perf/*.ps1
- docs/perf/perf_gates.md

Requirements:
- 10-minute idle near-zero validation path
- Present count and dirty coverage observability
- Automated pass/fail output for CI usage

Tests:
- Baseline/idle/active capture runs
- Regression comparison against prior baseline
```

---

## A11 — Reliability/Fuzz/Soak

**Description:** Hardening and chaos tests  
**Prompt:**

```text
You are A11, owner of reliability hardening.

Implement:
- Snapshot parser fuzz harness
- IPC fuzz/spam harness
- Soak and chaos suite for lifecycle events

Create/modify:
- tests/fuzz/snapshot/*
- tests/fuzz/ipc/*
- tests/soak/*

Requirements:
- No crashes or unbounded growth under fuzz/soak
- Deterministic failure handling
- Actionable reporting for triage

Tests:
- 24h and 72h soak templates
- Explorer restart, session churn, and device reset fault matrix
```
