# Runtime Resilience Feature Slice — Batch 3 (X1-701 / X1-702 / X1-703)

This document describes the runtime resilience foundation added in Batch 3:

- `src/runtime/StartupManager.h/.cpp`
- `src/runtime/StateStore.h/.cpp`
- `src/runtime/SessionManager.h/.cpp`

The slice is intentionally integration-ready and scheduler-agnostic. It focuses
on deterministic policy decisions, explicit state transitions, and portable
non-Windows stubs with `_WIN32` guarded Win32 mappings.

## 1) X1-701 — Startup and restart manager

### 1.1 Goals

`ntium::runtime::StartupManager` provides:

- startup policy abstraction (`StartupPolicy`, `RestartPolicy`)
- single-instance policy gate abstraction (`ISingleInstanceGate`)
- deterministic startup/restart decisions with bounded backoff behavior

### 1.2 Single-instance model

The startup path can enforce a process-level single-instance lease:

- abstraction: `ISingleInstanceGate`
- default implementation: `CreatePlatformSingleInstanceGate()`

Platform behavior:

- `_WIN32`: named mutex-backed gate (`CreateMutexA`, `WaitForSingleObject`)
- non-Windows: deterministic in-process lease set (test-friendly stub)

Decision outcomes explicitly encode duplicate-instance and unavailable-gate
conditions:

- `kDenyDuplicateInstance`
- `kDenyPolicySuppressed`

### 1.3 Restart policy model

`RestartPolicy` includes:

- `max_restarts_per_window`
- rolling window duration
- min/max backoff
- exponential multiplier

`StartupManager` tracks restart attempts in a rolling time window and emits:

- `StartupDecision` on launch evaluation
- `RestartDecision` on process exit

Exits are classified to support deterministic restart eligibility:

- restart candidates: crash/watchdog/policy-restart
- non-candidates: graceful/OS shutdown/unknown

## 2) X1-702 — Atomic state persistence and restore fallback

### 2.1 Goals

`ntium::runtime::StateStore` provides a simple durable store for runtime state
snapshots:

- atomic write path using `primary`, `temp`, and optional `backup`
- corruption detection via payload checksum
- restore fallback from backup
- rollback/repair semantics on write/read failure paths

### 2.2 Persist path (`PersistAtomic`)

Persist flow:

1. Serialize snapshot (schema/sequence/timestamp/payload/checksum)
2. Write temp file (`.tmp`)
3. Optionally read-back verify temp file
4. Optionally refresh backup from previous primary
5. Atomically replace primary from temp
6. On replace failure, attempt rollback from backup

Result surface includes:

- `status`
- `backup_updated`
- `rollback_applied`

### 2.3 Restore path (`RestoreWithFallback`)

Restore flow:

1. Attempt primary parse + checksum validation
2. If primary is corrupt, attempt backup
3. If backup succeeds, optionally repair primary from backup snapshot

Result surface includes:

- `used_backup`
- `repaired_primary`
- restored `StateSnapshot`

This supports corruption recovery requirements while preserving deterministic
fallback behavior.

## 3) X1-703 — Session and power transition manager

### 3.1 Goals

`ntium::runtime::SessionManager` provides:

- deterministic runtime/session lifecycle FSM
- explicit trigger handling for session and power events
- action hook contract for pause/resume/persist/restore/rebind operations

### 3.2 Runtime states and triggers

States:

- `kStopped`
- `kActive`
- `kLocked`
- `kSuspended`
- `kLogoffPending`
- `kShutdownPending`

Triggers:

- process/logon/logoff
- lock/unlock
- console/remote connect/disconnect
- suspend/resume
- shutdown

### 3.3 Deterministic transition planning

The transition table is centralized in `DeterminePlan(...)`:

- each trigger maps current state -> next state + ordered action mask
- invalid/no-op transitions are explicitly ignored
- action hook failures return `kHookFailed` and prevent state mutation

Action model:

- `kPauseRuntime`
- `kResumeRuntime`
- `kPersistState`
- `kRestoreState`
- `kRebindDesktop`

Actions are invoked in deterministic order:

1. persist
2. pause
3. rebind
4. restore
5. resume

### 3.4 Win32 mappings and non-Windows stubs

`SessionManager` provides direct helpers for host event integration:

- `HandleSessionChange(...)`
- `HandlePowerBroadcast(...)`
- `HandleEndSession(...)`

Platform mapping:

- `_WIN32`: maps WTS and power constants from Win32 headers
- non-Windows: uses deterministic local constant mappings for portable behavior

## 4) Integration notes

1. Wire `StartupManager` into process bootstrap and watchdog/relauncher glue.
2. Serialize camera/style/layout state into `StateSnapshot::payload`.
3. Bind `SessionTransitionHooks` to:
   - runtime pause/resume
   - state persist/restore
   - desktop host rebind (`H1-101/H1-103` integration path)
4. Feed host lifecycle events into session/power handlers for deterministic
   state progression.

## 5) Ticket mapping summary

- **X1-701**: startup policy + single-instance gate + restart backoff
- **X1-702**: atomic persist, backup fallback, rollback/repair behavior
- **X1-703**: session/power deterministic FSM with explicit transition hooks
