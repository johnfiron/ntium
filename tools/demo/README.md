# Live Validation Demo

This demo provides a runnable validation harness for the current foundation
implementation. It exercises real code paths (not mocks) for:

- snapshot parser (valid + corrupted input)
- IPC frame/protocol validation
- camera cursor-anchored zoom behavior
- overlay event store + throttle + dirty-region + monitor coalescing
- input router + IPC/user arbitration

## Build

```bash
mkdir -p tools/demo/bin tools/demo/out
g++ -std=c++20 -Wall -Wextra -Werror -I/workspace -I/workspace/src \
  tools/demo/live_validation_demo.cpp \
  src/scene/GeoMath.cpp src/scene/CameraController.cpp \
  src/ingest/SnapshotParser.cpp src/ipc/PipeProtocol.cpp \
  src/overlay/EventStore.cpp src/overlay/OverlayThrottle.cpp src/overlay/DirtyRegionGenerator.cpp \
  src/input/InputRouter.cpp src/input/ArbitrationEngine.cpp \
  src/render/DirtyRectCoalescer.cpp \
  -o tools/demo/bin/live_validation_demo
```

## Run

```bash
./tools/demo/bin/live_validation_demo \
  --json-out tools/demo/out/live_validation_report.json | tee tools/demo/out/live_validation_report.txt
```

Expected result:

- Exit code `0`
- `Passed 5 / 5 checks`

## Artifacts

- `tools/demo/out/live_validation_report.txt`
- `tools/demo/out/live_validation_report.json`

## Note

During live run setup, this harness exposed a numeric guard bug in
`src/scene/GeoMath.cpp` ray/ellipsoid intersection (false negative hits at
Earth-scale values). The demo includes regression coverage for that path.
