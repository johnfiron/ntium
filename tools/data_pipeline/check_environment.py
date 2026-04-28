#!/usr/bin/env python3
"""Environment and dependency checks for data pipeline."""

from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path


REQUIRED_BINS = [
    "ogr2ogr",
    "gdalwarp",
    "gdal_rasterize",
    "gdalinfo",
    "gdallocationinfo",
]

OPTIONAL_BINS = [
    "gdal_calc.py",
    "python3",
]


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    cfg = root / "tools" / "data_pipeline" / "config" / "pipeline_config.json"

    missing_required = [b for b in REQUIRED_BINS if shutil.which(b) is None]
    missing_optional = [b for b in OPTIONAL_BINS if shutil.which(b) is None]

    print("Data Pipeline Environment Check")
    print("===============================")
    print(f"workspace: {root}")
    print(f"config: {cfg} ({'present' if cfg.exists() else 'missing'})")

    if missing_required:
        print("missing required binaries:")
        for b in missing_required:
            print(f"- {b}")
    else:
        print("required binaries: OK")

    if missing_optional:
        print("missing optional binaries:")
        for b in missing_optional:
            print(f"- {b}")
    else:
        print("optional binaries: OK")

    if cfg.exists():
        try:
            json.loads(cfg.read_text(encoding="utf-8"))
            print("config json parse: OK")
        except json.JSONDecodeError as exc:
            print(f"config json parse: FAIL ({exc})")
            return 2
    else:
        print("config json parse: SKIPPED (missing config)")

    if missing_required:
        print("result: FAIL")
        return 1

    print("result: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
