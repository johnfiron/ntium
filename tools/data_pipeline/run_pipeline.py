#!/usr/bin/env python3
"""
Data pipeline orchestrator:
  1) Extract/normalize building heights
  2) Reproject sources
  3) Rasterize building heights
  4) Build DSM surface
  5) Emit tile manifests
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shlex
import subprocess
from pathlib import Path
from typing import Dict, Iterable, Set, Tuple


def run(cmd: list[str], *, dry_run: bool = False) -> None:
    print("$", " ".join(shlex.quote(c) for c in cmd))
    if dry_run:
        return
    subprocess.run(cmd, check=True)


def gdal_calc_command() -> str:
    """
    Choose an available gdal_calc entry point.
    """
    for candidate in ("gdal_calc.py", "gdal_calc"):
        try:
            subprocess.run(
                [candidate, "--help"],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return candidate
        except FileNotFoundError:
            continue
    raise RuntimeError(
        "Unable to locate gdal_calc executable (expected gdal_calc.py or gdal_calc)"
    )


def load_config(path: Path) -> Dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def quote_ident(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


def layer_fields(dataset_path: Path, layer_name: str) -> Set[str]:
    out = subprocess.check_output(
        ["ogrinfo", "-ro", "-so", "-json", str(dataset_path), layer_name],
        text=True,
    )
    data = json.loads(out)
    layers = data.get("layers", [])
    if not layers:
        raise RuntimeError(f"No layers found while inspecting {dataset_path}:{layer_name}")
    return {field["name"] for field in layers[0].get("fields", [])}


def tag_numeric_expr(tags_column: str, key: str) -> str:
    marker = f"'\"{key}\"=>\"'"
    extracted = (
        f"CASE WHEN instr({tags_column}, {marker}) > 0 THEN "
        f"substr(substr({tags_column}, instr({tags_column}, {marker}) + length({marker})), "
        f"1, instr(substr({tags_column}, instr({tags_column}, {marker}) + length({marker})), '\"') - 1) "
        f"END"
    )
    # Strip spaces/metric suffix to tolerate values like "12 m".
    return f"CAST(REPLACE(REPLACE({extracted}, 'm', ''), ' ', '') AS REAL)"


def height_select_sql(source_layer: str, fields: Iterable[str]) -> str:
    field_set = set(fields)
    terms: list[str] = []
    if "height" in field_set:
        terms.append(f"CAST({quote_ident('height')} AS REAL)")
    if "other_tags" in field_set:
        terms.append(tag_numeric_expr(quote_ident("other_tags"), "height"))
    if "building:levels" in field_set:
        terms.append(f"CAST({quote_ident('building:levels')} AS REAL) * 3.2")
    if "other_tags" in field_set:
        terms.append(f"{tag_numeric_expr(quote_ident('other_tags'), 'building:levels')} * 3.2")

    when_clauses = " ".join(f"WHEN {expr} > 0 THEN {expr}" for expr in terms)
    height_expr = f"CASE {when_clauses} ELSE 12.0 END" if when_clauses else "12.0"
    return f"SELECT *, {height_expr} AS height_m FROM {quote_ident(source_layer)}"


def map_extent_to_target(dem_src: Path, dem_reproj: Path, target_epsg: int, dry_run: bool) -> None:
    run(
        [
            "gdalwarp",
            "-t_srs",
            f"EPSG:{target_epsg}",
            str(dem_src),
            str(dem_reproj),
        ],
        dry_run=dry_run,
    )


def raster_bounds(path: Path) -> Tuple[float, float, float, float]:
    # Uses gdalinfo JSON output for robust parsing.
    out = subprocess.check_output(["gdalinfo", "-json", str(path)], text=True)
    data = json.loads(out)
    corners = data["cornerCoordinates"]
    xmin = float(corners["lowerLeft"][0])
    ymin = float(corners["lowerLeft"][1])
    xmax = float(corners["upperRight"][0])
    ymax = float(corners["upperRight"][1])
    return xmin, ymin, xmax, ymax


def extract_buildings_from_osm(
    osm_pbf: Path,
    raw_gpkg: Path,
    height_gpkg: Path,
    clipped_gpkg: Path,
    aoi_geojson: Path,
    dry_run: bool,
) -> None:
    run(
        [
            "ogr2ogr",
            "-f",
            "GPKG",
            str(raw_gpkg),
            str(osm_pbf),
            "multipolygons",
            "-where",
            "building IS NOT NULL",
            "-nln",
            "buildings_raw",
        ],
        dry_run=dry_run,
    )

    fields = layer_fields(raw_gpkg, "buildings_raw")
    run(
        [
            "ogr2ogr",
            "-f",
            "GPKG",
            str(height_gpkg),
            str(raw_gpkg),
            "-dialect",
            "SQLITE",
            "-sql",
            height_select_sql("buildings_raw", fields),
            "-nln",
            "buildings_height",
        ],
        dry_run=dry_run,
    )

    run(
        [
            "ogr2ogr",
            "-f",
            "GPKG",
            str(clipped_gpkg),
            str(height_gpkg),
            "-clipsrc",
            str(aoi_geojson),
            "-nln",
            "buildings_height",
        ],
        dry_run=dry_run,
    )


def extract_buildings_from_gpkg(
    src_gpkg: Path,
    src_layer: str,
    height_gpkg: Path,
    clipped_gpkg: Path,
    aoi_geojson: Path,
    dry_run: bool,
) -> None:
    fields = layer_fields(src_gpkg, src_layer)
    run(
        [
            "ogr2ogr",
            "-f",
            "GPKG",
            str(height_gpkg),
            str(src_gpkg),
            src_layer,
            "-dialect",
            "SQLITE",
            "-sql",
            height_select_sql(src_layer, fields),
            "-nln",
            "buildings_height",
        ],
        dry_run=dry_run,
    )

    run(
        [
            "ogr2ogr",
            "-f",
            "GPKG",
            str(clipped_gpkg),
            str(height_gpkg),
            "-clipsrc",
            str(aoi_geojson),
            "-nln",
            "buildings_height",
        ],
        dry_run=dry_run,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Run map/building height data pipeline")
    parser.add_argument(
        "--config",
        default="tools/data_pipeline/config/pipeline_config.example.json",
        help="Path to pipeline config JSON",
    )
    parser.add_argument("--dry-run", action="store_true", help="Print commands only")
    args = parser.parse_args()

    cfg = load_config(Path(args.config))
    data_cfg = cfg["data"]
    proc_cfg = cfg["processing"]

    raw_dir = Path(data_cfg["raw_dir"])
    processed_dir = Path(data_cfg["processed_dir"])
    output_dir = Path(data_cfg["output_dir"])
    ensure_dir(raw_dir)
    ensure_dir(processed_dir)
    ensure_dir(output_dir)

    aoi_geojson = Path(data_cfg["aoi_geojson"])
    dem_path = Path(data_cfg["dem_path"])
    source_type = data_cfg["buildings_source"]["type"]

    target_epsg = int(proc_cfg["target_epsg"])
    res_m = float(proc_cfg["raster_resolution_m"])

    raw_gpkg = processed_dir / "buildings_raw.gpkg"
    height_gpkg = processed_dir / "buildings_height.gpkg"
    clipped_gpkg = processed_dir / "buildings_height_clip.gpkg"
    buildings_3857 = processed_dir / "buildings_target.gpkg"
    dem_target = processed_dir / "dem_target.tif"
    building_height_raster = processed_dir / "building_height.tif"
    dsm_raster = processed_dir / "surface_dsm.tif"
    manifest_json = output_dir / "tile_manifest.json"

    if source_type == "osm_pbf":
        osm_pbf = Path(data_cfg["buildings_source"]["path"])
        extract_buildings_from_osm(
            osm_pbf, raw_gpkg, height_gpkg, clipped_gpkg, aoi_geojson, args.dry_run
        )
    elif source_type == "gpkg":
        src_gpkg = Path(data_cfg["buildings_source"]["path"])
        src_layer = data_cfg["buildings_source"]["layer"]
        extract_buildings_from_gpkg(
            src_gpkg, src_layer, height_gpkg, clipped_gpkg, aoi_geojson, args.dry_run
        )
    else:
        raise ValueError(f"Unsupported buildings_source.type: {source_type}")

    run(
        [
            "ogr2ogr",
            "-f",
            "GPKG",
            "-t_srs",
            f"EPSG:{target_epsg}",
            str(buildings_3857),
            str(clipped_gpkg),
            "-nln",
            "buildings_height",
        ],
        dry_run=args.dry_run,
    )

    map_extent_to_target(dem_path, dem_target, target_epsg, args.dry_run)

    if args.dry_run:
        print("Dry run complete (bounds not computed).")
        return 0

    xmin, ymin, xmax, ymax = raster_bounds(dem_target)

    run(
        [
            "gdal_rasterize",
            "-a",
            "height_m",
            "-tr",
            str(res_m),
            str(res_m),
            "-ot",
            "Float32",
            "-a_nodata",
            "0",
            "-te",
            str(xmin),
            str(ymin),
            str(xmax),
            str(ymax),
            str(buildings_3857),
            str(building_height_raster),
        ],
        dry_run=False,
    )

    calc_exec = gdal_calc_command()
    run(
        [
            calc_exec,
            "-A",
            str(dem_target),
            "-B",
            str(building_height_raster),
            "--calc=A+B",
            "--type=Float32",
            "--NoDataValue=0",
            f"--outfile={str(dsm_raster)}",
        ],
        dry_run=False,
    )

    # Emit a simple manifest for runtime/tile processing stages.
    tile_cfg = cfg["tiles"]
    tile_size_m = float(tile_cfg["tile_size_m"])
    cols = max(1, math.ceil((xmax - xmin) / tile_size_m))
    rows = max(1, math.ceil((ymax - ymin) / tile_size_m))
    manifest = {
        "target_epsg": target_epsg,
        "resolution_m": res_m,
        "extent": {"xmin": xmin, "ymin": ymin, "xmax": xmax, "ymax": ymax},
        "rows": rows,
        "cols": cols,
        "tile_size_m": tile_size_m,
        "assets": {
            "dem": str(dem_target),
            "building_height": str(building_height_raster),
            "dsm": str(dsm_raster),
            "buildings_vector": str(buildings_3857),
        },
    }
    with manifest_json.open("w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    print(f"Wrote tile manifest: {manifest_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
