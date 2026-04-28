#!/usr/bin/env python3
"""
Fetch USGS 3DEP 1/3 arc-second DEM tiles for an AOI and build a clipped DEM.
"""

from __future__ import annotations

import argparse
import math
import shlex
import subprocess
from pathlib import Path
from typing import Iterable, List, Set, Tuple
from urllib.error import HTTPError
from urllib.request import urlopen


def run(cmd: List[str]) -> None:
    print("$", " ".join(shlex.quote(c) for c in cmd))
    subprocess.run(cmd, check=True)


def aoi_extent(aoi_path: Path) -> Tuple[float, float, float, float]:
    out = subprocess.check_output(
        ["ogrinfo", "-ro", "-al", "-so", "-json", str(aoi_path)],
        text=True,
    )
    import json

    data = json.loads(out)
    layers = data.get("layers", [])
    if not layers:
        raise RuntimeError("No layers in AOI file.")
    extent = layers[0]["geometryFields"][0]["extent"]
    return (
        float(extent[0]),
        float(extent[1]),
        float(extent[2]),
        float(extent[3]),
    )


def tile_name(lat: int, lon: int) -> str:
    ns = "n" if lat >= 0 else "s"
    ew = "e" if lon >= 0 else "w"
    return f"{ns}{abs(lat):02d}{ew}{abs(lon):03d}"


def candidate_tiles(extent: Tuple[float, float, float, float]) -> Set[Tuple[int, int]]:
    xmin, ymin, xmax, ymax = extent
    # Conservative set of 1x1-degree tiles intersecting AOI bbox.
    lon_min = math.floor(xmin)
    lon_max = math.ceil(xmax) - 1
    lat_min = math.floor(ymin)
    lat_max = math.ceil(ymax) - 1
    tiles = set()
    for lat in range(lat_min, lat_max + 1):
        for lon in range(lon_min, lon_max + 1):
            tiles.add((lat, lon))
    return tiles


def tile_url(lat: int, lon: int) -> str:
    n = tile_name(lat, lon)
    return (
        "https://prd-tnm.s3.amazonaws.com/"
        f"StagedProducts/Elevation/13/TIFF/current/{n}/USGS_13_{n}.tif"
    )


def url_exists(url: str) -> bool:
    try:
        with urlopen(url, timeout=20) as resp:
            return resp.status == 200
    except HTTPError:
        return False
    except Exception:
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description="Fetch and clip 3DEP DEM tiles for AOI")
    parser.add_argument("--aoi", required=True, help="AOI GeoJSON path")
    parser.add_argument("--tiles-dir", default="data/raw/dem_tiles", help="DEM tile dir")
    parser.add_argument(
        "--out-dem",
        default="data/raw/dem.tif",
        help="Output clipped DEM path",
    )
    args = parser.parse_args()

    aoi_path = Path(args.aoi)
    tiles_dir = Path(args.tiles_dir)
    out_dem = Path(args.out_dem)
    work_dir = out_dem.parent / "_dem_work"
    tiles_dir.mkdir(parents=True, exist_ok=True)
    work_dir.mkdir(parents=True, exist_ok=True)
    out_dem.parent.mkdir(parents=True, exist_ok=True)

    extent = aoi_extent(aoi_path)
    tiles = sorted(candidate_tiles(extent))
    print(f"AOI extent: {extent}")
    print(f"Candidate tiles: {len(tiles)}")

    downloaded: List[Path] = []
    for lat, lon in tiles:
        n = tile_name(lat, lon)
        url = tile_url(lat, lon)
        dst = tiles_dir / f"USGS_13_{n}.tif"
        if dst.exists():
            downloaded.append(dst)
            print(f"tile exists: {dst.name}")
            continue
        if not url_exists(url):
            print(f"tile missing upstream: {n}")
            continue
        run(["curl", "-L", "-o", str(dst), url])
        downloaded.append(dst)

    if not downloaded:
        raise RuntimeError("No DEM tiles downloaded/found for AOI.")

    vrt = work_dir / "dem_mosaic.vrt"
    run(["gdalbuildvrt", str(vrt)] + [str(p) for p in downloaded])
    run(
        [
            "gdalwarp",
            "-cutline",
            str(aoi_path),
            "-crop_to_cutline",
            "-dstalpha",
            "-of",
            "GTiff",
            str(vrt),
            str(out_dem),
        ]
    )

    print(f"Wrote DEM: {out_dem}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
