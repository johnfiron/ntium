#!/usr/bin/env python3
"""
Generate QC report for building height and DSM outputs.
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
from pathlib import Path
from typing import Dict, List


def run_json(cmd: List[str]) -> Dict:
    out = subprocess.check_output(cmd, text=True)
    return json.loads(out)


def raster_stats(path: Path) -> Dict:
    info = run_json(["gdalinfo", "-json", "-stats", str(path)])
    bands = info.get("bands", [])
    if not bands:
        raise RuntimeError(f"No bands in raster: {path}")
    band0 = bands[0]
    return {
        "min": float(band0.get("minimum", 0.0)),
        "max": float(band0.get("maximum", 0.0)),
        "mean": float(band0.get("mean", 0.0)),
        "stddev": float(band0.get("stdDev", 0.0)),
        "size": info.get("size", []),
    }


def layer_stats(gpkg: Path, layer: str) -> Dict:
    sql_total = f"SELECT COUNT(*) AS n FROM {layer}"
    sql_height = f"SELECT COUNT(*) AS n FROM {layer} WHERE CAST(height AS REAL) > 0"
    sql_levels = (
        f"SELECT COUNT(*) AS n FROM {layer} "
        f"WHERE (CAST(height AS REAL) <= 0 OR height IS NULL) "
        f"AND CAST(\"building:levels\" AS REAL) > 0"
    )
    sql_default = (
        f"SELECT COUNT(*) AS n FROM {layer} "
        f"WHERE (CAST(height AS REAL) <= 0 OR height IS NULL) "
        f"AND (CAST(\"building:levels\" AS REAL) <= 0 OR \"building:levels\" IS NULL)"
    )
    sql_hmax = f"SELECT MAX(height_m) AS max_h FROM {layer}"
    sql_hmin = f"SELECT MIN(height_m) AS min_h FROM {layer}"
    sql_havg = f"SELECT AVG(height_m) AS avg_h FROM {layer}"

    def q(sql: str, key: str = "n") -> float:
        data = run_json(["ogrinfo", "-ro", "-al", "-q", "-dialect", "SQLITE", "-sql", sql, "-json", str(gpkg)])
        feats = data.get("features", [])
        if not feats:
            return 0.0
        props = feats[0].get("properties", {})
        value = props.get(key, 0.0)
        return float(value) if value is not None else 0.0

    total = q(sql_total)
    explicit_h = q(sql_height)
    levels_h = q(sql_levels)
    default_h = q(sql_default)
    max_h = q(sql_hmax, "max_h")
    min_h = q(sql_hmin, "min_h")
    avg_h = q(sql_havg, "avg_h")

    pct = lambda x: 0.0 if total <= 0 else (x * 100.0 / total)
    return {
        "total_buildings": int(total),
        "height_source": {
            "explicit_height_count": int(explicit_h),
            "levels_inferred_count": int(levels_h),
            "default_fallback_count": int(default_h),
            "explicit_height_pct": pct(explicit_h),
            "levels_inferred_pct": pct(levels_h),
            "default_fallback_pct": pct(default_h),
        },
        "height_m": {
            "min": min_h,
            "max": max_h,
            "avg": avg_h,
        },
    }


def seam_indicator(dsm_stats: Dict, dem_stats: Dict) -> Dict:
    # Lightweight seam proxy: compare stddev ratio and max spread.
    dem_std = max(1e-9, dem_stats["stddev"])
    dsm_std = max(1e-9, dsm_stats["stddev"])
    ratio = dsm_std / dem_std
    spread = dsm_stats["max"] - dsm_stats["min"]
    return {
        "stddev_ratio_dsm_over_dem": ratio,
        "dsm_value_spread": spread,
        "requires_visual_tile_seam_check": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate QC report for data pipeline outputs")
    parser.add_argument(
        "--config",
        default="tools/data_pipeline/config/pipeline_config.example.json",
        help="Pipeline config JSON path",
    )
    args = parser.parse_args()

    with Path(args.config).open("r", encoding="utf-8") as f:
        cfg = json.load(f)

    processed_dir = Path(cfg["data"]["processed_dir"])
    output_dir = Path(cfg["data"]["output_dir"])
    output_dir.mkdir(parents=True, exist_ok=True)

    buildings = processed_dir / "buildings_target.gpkg"
    dem = processed_dir / "dem_target.tif"
    bh = processed_dir / "building_height.tif"
    dsm = processed_dir / "surface_dsm.tif"

    report = {
        "artifacts": {
            "buildings_vector": str(buildings),
            "dem": str(dem),
            "building_height_raster": str(bh),
            "dsm": str(dsm),
        },
        "buildings": layer_stats(buildings, "buildings_height"),
        "rasters": {
            "dem": raster_stats(dem),
            "building_height": raster_stats(bh),
            "dsm": raster_stats(dsm),
        },
    }
    report["seam_proxy"] = seam_indicator(report["rasters"]["dsm"], report["rasters"]["dem"])

    # Basic flags
    max_height = report["buildings"]["height_m"]["max"]
    report["flags"] = {
        "suspicious_building_height_gt_800m": bool(max_height > 800.0),
        "empty_building_dataset": report["buildings"]["total_buildings"] == 0,
    }

    out_path = output_dir / "qc_report.json"
    with out_path.open("w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"Wrote QC report: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

