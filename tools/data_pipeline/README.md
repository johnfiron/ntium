## Data Pipeline (Map + Building Height Test Pack)

This folder provides a repeatable local pipeline to generate a basic map surface
with building heights for wallpaper runtime testing.

### Security / key policy

- **No API keys are stored in the repository.**
- Any key/token is read from environment variables only.
- `.env`, secret files, and local config overrides are git-ignored.

If a step requires a remote provider, export credentials in your shell only:

```bash
export OVERTURE_API_KEY="..."
export MAPTILER_API_KEY="..."
```

Do not write these values into files under the repo.

---

## Pipeline stages

1. **prepare**: validate toolchain and workspace paths
2. **derive-heights**: create building height attributes from raw vector data
3. **rasterize**: create building-height raster
4. **compose-surface**: combine DEM + building height into DSM
5. **qc**: emit coverage/range/alignment checks
6. **manifest**: create runtime tile/asset manifest

---

## Required inputs

- AOI polygon GeoJSON (`data/raw/aoi.geojson`)
- Building footprints vector (`data/raw/buildings.gpkg` or `.geojson`)
- DEM raster (`data/raw/dem.tif`)

### Required building fields

Recommended fields in building vector:

- `height` (meters, optional)
- `building:levels` (optional)
- `building` (string, optional)

Fallback policy is configurable and defaults to:

- explicit `height` if valid
- else `building:levels * 3.2`
- else `12.0` meters

---

## Quick start

```bash
python3 tools/data_pipeline/run_pipeline.py \
  --config tools/data_pipeline/config/pipeline_config.json
```

Outputs:

- `data/processed/demo_aoi/buildings_height.gpkg`
- `data/processed/demo_aoi/building_height.tif`
- `data/processed/demo_aoi/dsm_surface.tif`
- `data/processed/demo_aoi/qc_report.json`
- `data/processed/demo_aoi/runtime_manifest.json`

---

## Optional live provider download

Remote provider integration should enforce:

- key must come from environment variable
- key is never printed to logs
- key is never written to disk

If no key is set, download stage is skipped with a clear message.

## QC and secret scan

Generate QC report after pipeline run:

```bash
python3 tools/data_pipeline/qc_report.py \
  --config tools/data_pipeline/config/pipeline_config.json
```

Scan repository for accidentally committed keys:

```bash
python3 tools/data_pipeline/scan_for_secrets.py --root .
```

