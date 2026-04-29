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
  --config tools/data_pipeline/config/pipeline_config.example.json
```

Outputs:

- `data/processed/demo_aoi/buildings_height.gpkg`
- `data/processed/demo_aoi/building_height.tif`
- `data/processed/demo_aoi/dsm_surface.tif`
- `data/processed/demo_aoi/qc_report.json`
- `data/processed/demo_aoi/runtime_manifest.json`

---

## Maryland-first quick start (recommended on fresh install)

Use this first to keep storage/network usage low and validate the full
end-to-end pipeline before running the larger 4-state build.

### 1) Download Maryland extract + boundaries

```bash
mkdir -p data/raw/osm_states data/raw/boundaries
curl -L -o data/raw/osm_states/maryland-latest.osm.pbf https://download.geofabrik.de/north-america/us/maryland-latest.osm.pbf

curl -L -o data/raw/boundaries/cb_2023_us_state_500k.zip https://www2.census.gov/geo/tiger/GENZ2023/shp/cb_2023_us_state_500k.zip
unzip -o data/raw/boundaries/cb_2023_us_state_500k.zip -d data/raw/boundaries
ogr2ogr -f GeoJSON data/raw/aoi_maryland.geojson data/raw/boundaries/cb_2023_us_state_500k.shp \
  -dialect SQLITE \
  -sql "SELECT geometry FROM cb_2023_us_state_500k WHERE STUSPS = 'MD'"
```

### 2) Build Maryland buildings source

```bash
ogr2ogr -f GPKG data/raw/buildings_maryland.gpkg \
  data/raw/osm_states/maryland-latest.osm.pbf multipolygons \
  -where "building IS NOT NULL" \
  -nln buildings \
  -skipfailures
```

### 3) Fetch DEM for Maryland AOI

```bash
python3 tools/data_pipeline/fetch_dem_3dep.py \
  --aoi data/raw/aoi_maryland.geojson \
  --out-dem data/raw/dem_maryland.tif
```

### 4) Run Maryland config

```bash
cp tools/data_pipeline/config/pipeline_config.local.example.maryland.json \
   tools/data_pipeline/config/pipeline_config.local.json

python3 tools/data_pipeline/run_pipeline.py \
  --config tools/data_pipeline/config/pipeline_config.local.json
```

### 5) Expand to 4-state after Maryland passes

After Maryland artifacts validate successfully, switch to the 4-state template
in the next section.

---

## 4-state setup (Maryland, Delaware, Virginia, West Virginia)

The repo now includes helper scripts and config for your requested 4-state test region.

### 1) Download state extracts + boundaries

```bash
mkdir -p data/raw/osm_states data/raw/boundaries
curl -L -o data/raw/osm_states/maryland-latest.osm.pbf https://download.geofabrik.de/north-america/us/maryland-latest.osm.pbf
curl -L -o data/raw/osm_states/delaware-latest.osm.pbf https://download.geofabrik.de/north-america/us/delaware-latest.osm.pbf
curl -L -o data/raw/osm_states/virginia-latest.osm.pbf https://download.geofabrik.de/north-america/us/virginia-latest.osm.pbf
curl -L -o data/raw/osm_states/west-virginia-latest.osm.pbf https://download.geofabrik.de/north-america/us/west-virginia-latest.osm.pbf

curl -L -o data/raw/boundaries/cb_2023_us_state_500k.zip https://www2.census.gov/geo/tiger/GENZ2023/shp/cb_2023_us_state_500k.zip
unzip -o data/raw/boundaries/cb_2023_us_state_500k.zip -d data/raw/boundaries
ogr2ogr -f GeoJSON data/raw/aoi_4states.geojson data/raw/boundaries/cb_2023_us_state_500k.shp \
  -dialect SQLITE \
  -sql "SELECT ST_Union(geometry) AS geometry FROM cb_2023_us_state_500k WHERE STUSPS IN ('MD','DE','VA','WV')"
```

### 2) Build merged buildings source

```bash
python3 tools/data_pipeline/build_4state_buildings.py \
  --osm-dir data/raw/osm_states \
  --out-gpkg data/raw/buildings.gpkg
```

### 3) Fetch and clip 3DEP DEM

```bash
python3 tools/data_pipeline/fetch_dem_3dep.py \
  --aoi data/raw/aoi_4states.geojson \
  --out-dem data/raw/dem.tif
```

### 4) Create local config and run pipeline

```bash
cp tools/data_pipeline/config/pipeline_config.local.example.4states.json \
   tools/data_pipeline/config/pipeline_config.local.json

python3 tools/data_pipeline/run_pipeline.py \
  --config tools/data_pipeline/config/pipeline_config.local.json
```

Notes:

- `pipeline_config.local.json` is git-ignored by policy.
- The 4-state AOI is large. Start with `raster_resolution_m: 10.0` before trying finer resolutions.

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

