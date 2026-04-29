# Phase 2 Cloud Agent Prompt Library (Desktop Demo Integration)

These prompts are for the **Phase 2 objective**:
deliver a watchable Windows desktop map demo by integrating existing subsystems
into a runnable host executable.

---

## A0 — Program Architect / Integration Gate Owner

```text
You are A0. Own Phase 2 integration gates and cross-stream acceptance criteria.

Goals:
1) Finalize gate checklist for D0..D4 (data, bootstrap, desktop host, demo wiring, perf sanity).
2) Validate dependency completeness in ticket board before each batch.
3) Approve merge sequencing and block if deterministic criteria are missing.

Deliverables:
- docs/program/phase2_gates.md
- final D4 readiness declaration
```

---

## A12 — Data Pipeline Hardening

```text
You are A12. Harden geodata pipeline for 4-state AOI reliability.

Implement:
- Invalid geometry hardening (make-valid path and skip-failure accounting).
- Stable clipping behavior for large OSM building datasets.
- Per-run report for retained/skipped/invalid feature counts.

Files:
- tools/data_pipeline/run_pipeline.py
- tools/data_pipeline/qc_report.py
- tools/data_pipeline/README.md

Acceptance:
- Non-dry 4-state run finishes and emits all expected outputs.
- No fatal topology abort halts the full run.
```

---

## A13 — App Host Bootstrap (WinMain)

```text
You are A13. Create the integrated Win32 app entrypoint and runtime bootstrap.

Implement:
- apps/wallpaper_host/main_win32.cpp with wWinMain + message pump.
- src/app/AppHost.* and src/app/RuntimeGraph.* for init/shutdown ordering.
- Wiring hooks for DesktopHost, render runtime, ingest, IPC, and state restore.

Acceptance:
- Host executable launches on Windows and exits cleanly.
- Deterministic startup logs show subsystem init ordering.
```

---

## A14 — Desktop Runtime Integration

```text
You are A14. Integrate desktop host behavior for Win11 25H2 and first visible frame.

Implement:
- DefView-relative attach flow from app bootstrap.
- Icon ownership-safe hide/restore integrated into lifecycle.
- Explorer restart/session/display rebind hooks from integrated host app.

Acceptance:
- On Windows 11 25H2, app attaches and renders behind desktop interaction model.
- Rebind path works after explorer restart without leaks.
```

---

## A15 — Render + Asset Loader Integration

```text
You are A15. Wire processed map assets into renderer for first watchable 3D frame.

Implement:
- Manifest loader for data/processed runtime artifacts.
- Terrain + building height ingestion to render scene.
- Dirty-present integration for event-driven redraw only.

Acceptance:
- Host app renders a stable 3D map frame using generated assets.
- Camera interaction invalidates and redraws correctly without continuous loop.
```

---

## A16 — Controls + Overlay Integration

```text
You are A16. Integrate input, IPC, overlay updates into the host runtime.

Implement:
- Input router integration for orbit/pan/dolly interaction.
- Named pipe command path integration with ACK/ERROR responses.
- Overlay event update path with <=10Hz throttle under burst.

Acceptance:
- User input and IPC both control camera/style deterministically.
- Overlay updates appear and are throttled under burst load.
```

---

## A17 — Demo Validation + Runbook

```text
You are A17. Produce operator-ready desktop demo runbook and validation bundle.

Implement:
- End-to-end demo script: start host, load assets, interact, send IPC, push overlay event.
- Validation checklist for visible success criteria.
- Troubleshooting table for common attach/render/data failures.

Deliverables:
- docs/demo/desktop_demo_runbook.md
- docs/demo/desktop_demo_validation.md
```

---

## A10 — Perf Sanity on Integrated Host

```text
You are A10. Re-run perf sanity on the integrated host executable.

Implement:
- Minimal idle/active capture scripts targeting Phase 2 host.
- Sanity thresholds and pass/fail report output.

Acceptance:
- Idle event-driven behavior confirmed (no free-running present loop).
- Active interaction remains responsive with bounded frame times.
```
