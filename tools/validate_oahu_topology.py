#!/usr/bin/env python3
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "oahu_topology.h"
DEBUG_DIR = ROOT / "build" / "oahu_debug"


def read_const(text: str, name: str, kind: str):
    pattern = rf"constexpr\s+{kind}\s+{name}\s+=\s+([-0-9.]+)f?;"
    match = re.search(pattern, text)
    if not match:
        raise RuntimeError(f"missing generated constant: {name}")
    value = match.group(1)
    return float(value) if kind == "float" else int(float(value))


def main() -> int:
    require_debug_artifacts = "--require-debug-artifacts" in sys.argv
    text = HEADER.read_text(encoding="utf-8")

    checks = [
        (
            "coastline source",
            "Hawaii Statewide GIS Program Coastline layer" in text,
        ),
        (
            "elevation source",
            "USGS National Map Elevation Point Query Service" in text,
        ),
        (
            "coastline samples",
            read_const(text, "kOahuCoastlinePointCount", "int") >= 512,
        ),
        (
            "source detail",
            read_const(text, "kOahuSourceCoastlinePointCount", "int") >= 10000,
        ),
        (
            "grid density",
            read_const(text, "kOahuGridWidth", "int") * read_const(text, "kOahuGridHeight", "int") >= 2500,
        ),
        (
            "longitude bounds",
            read_const(text, "kOahuMinLongitude", "float") < -158.25
            and read_const(text, "kOahuMaxLongitude", "float") > -157.66,
        ),
        (
            "latitude bounds",
            read_const(text, "kOahuMinLatitude", "float") < 21.26
            and read_const(text, "kOahuMaxLatitude", "float") > 21.70,
        ),
        (
            "map aspect",
            1.10 <= read_const(text, "kOahuMapAspect", "float") <= 1.45,
        ),
        (
            "source area",
            590.0 <= read_const(text, "kOahuSourceAreaSquareMiles", "float") <= 605.0,
        ),
        (
            "sampled elevation",
            read_const(text, "kOahuMaxElevationMeters", "float") >= 900.0,
        ),
        (
            "landmark controls",
            read_const(text, "kOahuLandmarkCount", "int") >= 6
            and all(
                name in text
                for name in (
                    "Kaena Point",
                    "Kahuku Point",
                    "Mokapu Point",
                    "Koko Head",
                    "Pearl Harbor",
                    "Barbers Point",
                )
            ),
        ),
    ]

    if require_debug_artifacts:
        checks.extend(
            [
                (
                    "debug source artifact",
                    (DEBUG_DIR / "oahu_source.geojson").exists(),
                ),
                (
                    "debug resampled artifact",
                    (DEBUG_DIR / "oahu_resampled.geojson").exists(),
                ),
                (
                    "debug preview artifact",
                    (DEBUG_DIR / "oahu_preview.svg").exists(),
                ),
            ]
        )

    failed = [name for name, passed in checks if not passed]
    if failed:
        for name in failed:
            print(f"[FAIL] {name}", file=sys.stderr)
        return 1

    for name, _ in checks:
        print(f"[OK] {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
