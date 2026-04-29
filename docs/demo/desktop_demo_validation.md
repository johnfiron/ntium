# Desktop Demo Validation Matrix (V2-002 / G6-001)

This checklist is the operator evidence pack for Phase 2 watchable desktop demo gates.

## 1) Required command evidence

Run from repo root:

1. `g++ -std=c++20 -Wall -Wextra -Werror -I/workspace -I/workspace/src -c src/app/AppHost.cpp -o /tmp/AppHost.o`
2. `mkdir -p tools/demo/bin tools/demo/out && g++ -std=c++20 -Wall -Wextra -Werror -I/workspace -I/workspace/src tools/demo/live_validation_demo.cpp src/scene/GeoMath.cpp src/scene/CameraController.cpp src/ingest/SnapshotParser.cpp src/ipc/PipeProtocol.cpp src/overlay/EventStore.cpp src/overlay/OverlayThrottle.cpp src/overlay/DirtyRegionGenerator.cpp src/input/InputRouter.cpp src/input/ArbitrationEngine.cpp src/render/DirtyRectCoalescer.cpp -o tools/demo/bin/live_validation_demo`
3. `./tools/demo/bin/live_validation_demo --json-out tools/demo/out/live_validation_report.json | tee tools/demo/out/live_validation_report.txt`

Expected:

- Command 1 succeeds.
- Command 2 succeeds.
- Command 3 exits `0` and reports `Passed 5 / 5 checks`.

## 2) Perf sanity command

When metrics are available:

`python3 tools/perf/check_thresholds.py --input <metrics.json-or-csv> --output-json <gate_results.json>`

Expected:

- Exit code `0`.
- Output marks `overall_pass: true`.

## 3) Manual Windows validation checklist

1. Launch `wallpaper_host.exe` from built Windows artifact.
2. Confirm first watchable frame appears behind desktop icons.
3. Hold left mouse and move pointer: orbit changes view.
4. Mouse wheel: dolly/zoom updates camera.
5. Send IPC commands (`zoom`, `rotate`, `style`, `span`, `mirror`):
   - Valid command => deterministic `ACK`.
   - Invalid command => deterministic `ERROR`.
6. Feed overlay snapshot burst >10Hz:
   - Display updates continue.
   - Present cadence is throttled (latest-state wins).
7. Trigger lock/unlock and suspend/resume smoke:
   - Host recovers without restart.
8. Trigger monitor topology change:
   - Host rebinds and resumes rendering.

## 4) Gate mapping

- `R2-002`: no idle free-running present loop.
- `C2-001/C2-002/C2-003`: input + IPC + arbitration deterministic behavior.
- `O2-001/O2-002`: snapshot/overlay wiring + <=10Hz throttle.
- `X2-001/X2-002`: session/power + monitor/device churn recovery.
- `V2-001/V2-002`: runbook + perf sanity evidence.
- `G6-001`: full watchable demo checklist pass.
