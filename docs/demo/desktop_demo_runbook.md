# Desktop Demo Runbook (V2-001)

This runbook executes the first watchable desktop demo path with deterministic checks.

## 1) Data readiness (full 4-state pipeline)

Use the original full MD+DE+VA+WV path. Run only when outputs are missing/stale.

1. Ensure local config exists:
   - `cp tools/data_pipeline/config/pipeline_config.local.example.4states.json tools/data_pipeline/config/pipeline_config.local.json`
2. Build merged buildings:
   - `python3 tools/data_pipeline/build_4state_buildings.py --osm-dir data/raw/osm_states --out-gpkg data/raw/buildings.gpkg`
3. Fetch/clip DEM:
   - `python3 tools/data_pipeline/fetch_dem_3dep.py --aoi data/raw/aoi_4states.geojson --out-dem data/raw/dem.tif`
4. Run full pipeline:
   - `python3 tools/data_pipeline/run_pipeline.py --config tools/data_pipeline/config/pipeline_config.local.json`

Required outputs:

- `data/processed/4states/buildings_target.gpkg`
- `data/processed/4states/dem_target.tif`
- `data/processed/4states/building_height.tif`
- `data/processed/4states/surface_dsm.tif`
- `data/processed/4states/out/tile_manifest.json`

## 2) Build validation harness

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

## 3) Run validation harness

```bash
./tools/demo/bin/live_validation_demo \
  --json-out tools/demo/out/live_validation_report.json | tee tools/demo/out/live_validation_report.txt
```

Expected: `Passed 5 / 5 checks`.

## 4) Windows desktop execution steps

1. Build `wallpaper_host.exe` in Visual Studio (x64, Release).
2. Launch `wallpaper_host.exe`.
3. Verify first visible frame on desktop.
4. Hold left mouse and drag to orbit.
5. Use mouse wheel to dolly.
6. Send pipe command frames to `\\.\pipe\ntium.wallpaper.ctrl.v1`; confirm deterministic ACK/ERROR.
7. Feed snapshot updates and verify overlay changes are visible and throttled to <=10Hz under burst.
8. Validate idle behavior stays event-driven (no free-running present loop).

## 5) Evidence bundle

Collect:

- `tools/demo/out/live_validation_report.txt`
- `tools/demo/out/live_validation_report.json`
- Perf gate output json from `tools/perf/check_thresholds.py`
- Windows desktop demo capture (video/screenshot)
