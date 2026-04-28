#!/usr/bin/env python3
"""
Build a unified buildings.gpkg from MD/DE/VA/WV Geofabrik OSM PBF extracts.
"""

from __future__ import annotations

import argparse
import shlex
import subprocess
from pathlib import Path


def run(cmd: list[str]) -> None:
    print("$", " ".join(shlex.quote(c) for c in cmd))
    subprocess.run(cmd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build 4-state buildings GeoPackage")
    parser.add_argument(
        "--osm-dir",
        default="data/raw/osm_states",
        help="Directory containing state .osm.pbf files",
    )
    parser.add_argument(
        "--out-gpkg",
        default="data/raw/buildings.gpkg",
        help="Output buildings GeoPackage path",
    )
    args = parser.parse_args()

    osm_dir = Path(args.osm_dir)
    out_gpkg = Path(args.out_gpkg)
    out_gpkg.parent.mkdir(parents=True, exist_ok=True)

    state_files = [
        osm_dir / "maryland-latest.osm.pbf",
        osm_dir / "delaware-latest.osm.pbf",
        osm_dir / "virginia-latest.osm.pbf",
        osm_dir / "west-virginia-latest.osm.pbf",
    ]
    missing = [str(p) for p in state_files if not p.exists()]
    if missing:
        raise FileNotFoundError(f"Missing state extracts: {missing}")

    if out_gpkg.exists():
        out_gpkg.unlink()

    first = True
    for src in state_files:
        cmd = [
            "ogr2ogr",
            "-f",
            "GPKG",
            str(out_gpkg),
            str(src),
            "multipolygons",
            "-where",
            "building IS NOT NULL",
            "-nln",
            "buildings",
            "-skipfailures",
        ]
        if not first:
            cmd.insert(6, "-update")
            cmd.insert(7, "-append")
        run(cmd)
        first = False

    run(["ogrinfo", "-ro", "-so", str(out_gpkg), "buildings"])
    print(f"Wrote merged buildings: {out_gpkg}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
