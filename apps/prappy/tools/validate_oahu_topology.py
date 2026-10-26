#!/usr/bin/env python3
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "oahu_topology.h"
OAHU_RENDERER = ROOT / "src" / "visualizations" / "oahu_flyover.cpp"
DEBUG_DIR = ROOT / "build" / "oahu_debug"


def read_const(text: str, name: str, kind: str):
    pattern = rf"constexpr\s+{kind}\s+{name}\s+=\s+([-0-9.]+)f?;"
    match = re.search(pattern, text)
    if not match:
        raise RuntimeError(f"missing generated constant: {name}")
    value = match.group(1)
    return float(value) if kind == "float" else int(float(value))


def read_landmarks(text: str) -> dict:
    pattern = re.compile(
        r'\{\s*"([^"]+)",\s*([-0-9.]+)f,\s*([-0-9.]+)f,\s*'
        r'([-0-9.]+)f,\s*([-0-9.]+)f,\s*([01])\s*\}'
    )
    landmarks = {}
    for match in pattern.finditer(text):
        landmarks[match.group(1)] = {
            "lon": float(match.group(2)),
            "lat": float(match.group(3)),
            "x": float(match.group(4)),
            "y": float(match.group(5)),
            "land": int(match.group(6)),
        }
    return landmarks


def max_land_elevation_sample(text: str):
    terrain_start = text.index("inline constexpr std::array<OahuTerrainSample")
    pattern = re.compile(r"\{\s*([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f,\s*([01])\s*\}")
    best = None
    for match in pattern.finditer(text[terrain_start:]):
        sample = {
            "x": float(match.group(1)),
            "y": float(match.group(2)),
            "elevation": float(match.group(3)),
            "land": int(match.group(4)),
        }
        if sample["land"] and (best is None or sample["elevation"] > best["elevation"]):
            best = sample
    return best


def main() -> int:
    require_debug_artifacts = "--require-debug-artifacts" in sys.argv
    text = HEADER.read_text(encoding="utf-8")
    renderer_text = OAHU_RENDERER.read_text(encoding="utf-8")
    landmarks = read_landmarks(text)
    peak = max_land_elevation_sample(text)

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
            read_const(text, "kOahuCoastlinePointCount", "int") >= 4096,
        ),
        (
            "source detail",
            read_const(text, "kOahuSourceCoastlinePointCount", "int") >= 10000,
        ),
        (
            "grid density",
            read_const(text, "kOahuGridWidth", "int") * read_const(text, "kOahuGridHeight", "int") >= 43000,
        ),
        (
            "elevation smoothing",
            read_const(text, "kOahuElevationSmoothingPasses", "int") >= 2,
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
        (
            "landmark east-west axis",
            landmarks["Kaena Point"]["x"]
            < landmarks["Barbers Point"]["x"]
            < landmarks["Kahuku Point"]["x"]
            < landmarks["Mokapu Point"]["x"]
            < landmarks["Koko Head"]["x"],
        ),
        (
            "landmark north-south axis",
            landmarks["Koko Head"]["y"]
            < landmarks["Barbers Point"]["y"]
            < landmarks["Pearl Harbor"]["y"]
            < landmarks["Mokapu Point"]["y"]
            < landmarks["Kaena Point"]["y"]
            < landmarks["Kahuku Point"]["y"],
        ),
        (
            "peak location",
            peak is not None
            and 0.18 <= peak["x"] <= 0.34
            and 0.45 <= peak["y"] <= 0.65,
        ),
        (
            "render-space north axis",
            "World axes: X east, Y elevation, Z north." in renderer_text
            and "const float z = (yValue - 0.5f) * zSpan;" in renderer_text,
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
