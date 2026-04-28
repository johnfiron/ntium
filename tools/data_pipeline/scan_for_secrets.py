#!/usr/bin/env python3
"""
Lightweight repo secret scanner for common API/token patterns.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import List, Tuple


PATTERNS: List[Tuple[str, str]] = [
    ("openai_like_key", r"\bsk-[A-Za-z0-9]{20,}\b"),
    ("aws_access_key_id", r"\bAKIA[0-9A-Z]{16}\b"),
    ("github_pat", r"\bghp_[A-Za-z0-9]{30,}\b"),
    ("generic_api_key_assignment", r"(?i)\b(api[_-]?key|token|secret)\b\s*[:=]\s*['\"][^'\"]{12,}['\"]"),
    ("private_key_block", r"-----BEGIN (RSA|EC|DSA|OPENSSH|PGP) PRIVATE KEY-----"),
]

SKIP_DIRS = {
    ".git",
    "node_modules",
    ".venv",
    "venv",
    "__pycache__",
    "tmp",
    "artifacts",
}

SKIP_SUFFIXES = {
    ".png",
    ".jpg",
    ".jpeg",
    ".gif",
    ".webp",
    ".tif",
    ".tiff",
    ".zip",
    ".gz",
    ".bz2",
    ".xz",
    ".7z",
    ".bin",
    ".exe",
    ".dll",
    ".so",
    ".o",
    ".a",
    ".pbf",
}


def iter_files(root: Path):
    for p in root.rglob("*"):
        if not p.is_file():
            continue
        if any(part in SKIP_DIRS for part in p.parts):
            continue
        if p.suffix.lower() in SKIP_SUFFIXES:
            continue
        yield p


def scan_file(path: Path):
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except Exception:
        return []
    hits = []
    for name, pattern in PATTERNS:
        for m in re.finditer(pattern, text):
            line = text.count("\n", 0, m.start()) + 1
            hits.append({"pattern": name, "line": line})
    return hits


def main() -> int:
    parser = argparse.ArgumentParser(description="Scan repository for common secret patterns")
    parser.add_argument("--root", default=".", help="Root directory to scan")
    parser.add_argument("--output", default="", help="Optional JSON output file")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    findings = []
    for p in iter_files(root):
        hits = scan_file(p)
        if hits:
            findings.append({"file": str(p.relative_to(root)), "hits": hits})

    report = {"root": str(root), "finding_count": len(findings), "findings": findings}
    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=2), encoding="utf-8")

    if findings:
        print(f"secret scan: FAIL ({len(findings)} files with findings)")
        for f in findings[:20]:
            print(f"- {f['file']}: {len(f['hits'])} hits")
        if len(findings) > 20:
            print(f"... and {len(findings)-20} more files")
        return 1

    print("secret scan: PASS (no matches)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

