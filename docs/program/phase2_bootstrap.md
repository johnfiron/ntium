# Phase 2 Bootstrap Order (Batch 1)

This note documents the deterministic runtime bootstrap/shutdown order introduced
for tickets A2-001 and A2-002.

## Initialization order

`AppHost` builds a `RuntimeGraph` with these nodes in fixed order:

1. `startup_manager`
2. `session_manager`
3. `desktop_host`
4. `monitor_manager`
5. `render_runtime`
6. `input_runtime`
7. `ipc_runtime`
8. `ingest_runtime`

If any initialization step fails, `RuntimeGraph` immediately rolls back by
shutting down already-initialized nodes in reverse order.

## Shutdown order

Normal shutdown and rollback both use reverse order:

1. `ingest_runtime`
2. `ipc_runtime`
3. `input_runtime`
4. `render_runtime`
5. `monitor_manager`
6. `desktop_host`
7. `session_manager`
8. `startup_manager`

## Current scaffold behavior

- Windows-only integration points (`DesktopHost`, `MonitorManager`, `wWinMain`)
  are guarded with `#if defined(_WIN32)`.
- Non-Windows builds keep a clean scaffold path with explicit notes for skipped
  platform-only nodes.
- Components that are not fully wired yet use explicit TODO-style failure or
  note paths (for example IPC runtime loop and live ingest watcher integration),
  so behavior is visible and not silently ignored.
