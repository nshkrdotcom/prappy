#!/usr/bin/env python3
import json
import math
import sys
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "src" / "oahu_topology.h"
DEBUG_DIR = ROOT / "build" / "oahu_debug"
USER_AGENT = "prappy-native-oahu-data/1.1 (local development)"

GRID_WIDTH = 241
GRID_HEIGHT = 181
COASTLINE_POINTS = 4096
ELEVATION_SMOOTHING_PASSES = 2
BOUND_PADDING_FRACTION = 0.06
EARTH_RADIUS_METERS = 6371008.8

COASTLINE_QUERY_URL = (
    "https://geodata.hawaii.gov/arcgis/rest/services/Terrestrial/MapServer/3/query"
)

LANDMARKS = [
    {"name": "Kaena Point", "lon": -158.28058, "lat": 21.57442},
    {"name": "Kahuku Point", "lon": -157.9839349, "lat": 21.7119767},
    {"name": "Mokapu Point", "lon": -157.7229452, "lat": 21.4584208},
    {"name": "Koko Head", "lon": -157.704135, "lat": 21.262935},
    {"name": "Pearl Harbor", "lon": -157.980240, "lat": 21.351183},
    {"name": "Barbers Point", "lon": -158.115036, "lat": 21.320634},
]


def fetch_json(url: str) -> dict:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.loads(response.read().decode("utf-8"))


def coastline_query_url() -> str:
    query = urllib.parse.urlencode(
        {
            "where": "isle='Oahu' AND sqmi > 500",
            "outFields": "*",
            "returnGeometry": "true",
            "outSR": "4326",
            "f": "geojson",
        }
    )
    return f"{COASTLINE_QUERY_URL}?{query}"


def ring_area(points) -> float:
    area = 0.0
    for current, following in zip(points, points[1:] + points[:1]):
        area += current[0] * following[1] - following[0] * current[1]
    return area * 0.5


def fetch_oahu_polygon():
    data = fetch_json(coastline_query_url())
    features = data.get("features", [])
    if not features:
        raise RuntimeError("Hawaii GIS coastline query returned no Oahu feature")

    feature = max(features, key=lambda item: item.get("properties", {}).get("sqmi", 0.0))
    geometry = feature.get("geometry", {})
    if geometry.get("type") != "Polygon":
        raise RuntimeError(f"Expected Polygon, got {geometry.get('type')}")

    rings = geometry.get("coordinates", [])
    if not rings:
        raise RuntimeError("Oahu coastline feature has no rings")

    outer = max(rings, key=lambda ring: abs(ring_area(ring)))
    holes = [ring for ring in rings if ring is not outer]
    return feature, outer, holes


def projection_origin(rings):
    points = [point for ring in rings for point in ring]
    lons = [point[0] for point in points]
    lats = [point[1] for point in points]
    return (min(lons) + max(lons)) * 0.5, (min(lats) + max(lats)) * 0.5


def project(lon: float, lat: float, origin_lon: float, origin_lat: float):
    origin_lat_rad = math.radians(origin_lat)
    x = math.radians(lon - origin_lon) * EARTH_RADIUS_METERS * math.cos(origin_lat_rad)
    y = math.radians(lat - origin_lat) * EARTH_RADIUS_METERS
    return x, y


def unproject(x: float, y: float, origin_lon: float, origin_lat: float):
    origin_lat_rad = math.radians(origin_lat)
    lon = origin_lon + math.degrees(x / (EARTH_RADIUS_METERS * math.cos(origin_lat_rad)))
    lat = origin_lat + math.degrees(y / EARTH_RADIUS_METERS)
    return lon, lat


def point_in_ring(x: float, y: float, ring) -> bool:
    inside = False
    j = len(ring) - 1
    for i, point in enumerate(ring):
        xi, yi = point
        xj, yj = ring[j]
        crosses = (yi > y) != (yj > y)
        if crosses:
            x_at_y = (xj - xi) * (y - yi) / ((yj - yi) or 1.0e-12) + xi
            if x < x_at_y:
                inside = not inside
        j = i
    return inside


def point_on_land(x: float, y: float, outer, holes) -> bool:
    if not point_in_ring(x, y, outer):
        return False
    return not any(point_in_ring(x, y, hole) for hole in holes)


def normalized_xy(x: float, y: float, bounds):
    min_x, min_y, max_x, max_y = bounds
    return (x - min_x) / (max_x - min_x), (y - min_y) / (max_y - min_y)


def denormalized_xy(x: float, y: float, bounds):
    min_x, min_y, max_x, max_y = bounds
    return min_x + x * (max_x - min_x), min_y + y * (max_y - min_y)


def projected_bounds(points):
    xs = [point[0] for point in points]
    ys = [point[1] for point in points]
    width = max(xs) - min(xs)
    height = max(ys) - min(ys)
    pad = max(width, height) * BOUND_PADDING_FRACTION
    return min(xs) - pad, min(ys) - pad, max(xs) + pad, max(ys) + pad


def sample_closed_ring(points, count, bounds):
    ring = list(points)
    if ring[0] != ring[-1]:
        ring.append(ring[0])

    distances = [0.0]
    total = 0.0
    for a, b in zip(ring, ring[1:]):
        total += math.dist(a, b)
        distances.append(total)

    sampled = []
    segment = 0
    for i in range(count):
        target = total * i / count
        while segment + 1 < len(distances) and distances[segment + 1] < target:
            segment += 1

        start = ring[segment]
        end = ring[(segment + 1) % len(ring)]
        segment_length = distances[segment + 1] - distances[segment]
        t = 0.0 if segment_length <= 0.0 else (target - distances[segment]) / segment_length
        x = start[0] + (end[0] - start[0]) * t
        y = start[1] + (end[1] - start[1]) * t
        sampled.append(normalized_xy(x, y, bounds))

    return sampled


def build_landmarks(bounds, origin_lon, origin_lat, outer, holes):
    landmarks = []
    for landmark in LANDMARKS:
        x, y = project(landmark["lon"], landmark["lat"], origin_lon, origin_lat)
        nx, ny = normalized_xy(x, y, bounds)
        landmarks.append(
            {
                "name": landmark["name"],
                "lon": landmark["lon"],
                "lat": landmark["lat"],
                "x": nx,
                "y": ny,
                "land": point_on_land(x, y, outer, holes),
            }
        )
    return landmarks


def fetch_elevation_meters(lon: float, lat: float) -> float:
    query = urllib.parse.urlencode(
        {
            "x": f"{lon:.7f}",
            "y": f"{lat:.7f}",
            "units": "Meters",
            "wkid": "4326",
        }
    )
    url = f"https://epqs.nationalmap.gov/v1/json?{query}"
    last_error = None
    for attempt in range(4):
        try:
            data = fetch_json(url)
            value = data.get("value")
            if value is None:
                return 0.0
            return max(float(value), 0.0)
        except Exception as exc:
            last_error = exc
            time.sleep(0.35 * (attempt + 1))

    print(f"warning: elevation lookup failed for {lon},{lat}: {last_error}", file=sys.stderr)
    return 0.0


def build_grid(outer, holes, bounds, origin_lon, origin_lat):
    samples = []
    jobs = {}
    with ThreadPoolExecutor(max_workers=16) as executor:
        for row in range(GRID_HEIGHT):
            for col in range(GRID_WIDTH):
                x = bounds[0] + (bounds[2] - bounds[0]) * col / (GRID_WIDTH - 1)
                y = bounds[1] + (bounds[3] - bounds[1]) * row / (GRID_HEIGHT - 1)
                lon, lat = unproject(x, y, origin_lon, origin_lat)
                nx, ny = normalized_xy(x, y, bounds)
                land = point_on_land(x, y, outer, holes)
                sample = {
                    "x": nx,
                    "y": ny,
                    "lon": lon,
                    "lat": lat,
                    "land": land,
                    "elevation": 0.0,
                }
                samples.append(sample)
                if land:
                    jobs[executor.submit(fetch_elevation_meters, lon, lat)] = sample

        completed = 0
        total = len(jobs)
        for future in as_completed(jobs):
            jobs[future]["elevation"] = future.result()
            completed += 1
            if completed % 250 == 0 or completed == total:
                print(f"elevation samples: {completed}/{total}")

    return samples


def smooth_elevations(samples):
    for _ in range(ELEVATION_SMOOTHING_PASSES):
        source = [sample["elevation"] for sample in samples]
        for row in range(GRID_HEIGHT):
            for col in range(GRID_WIDTH):
                index = row * GRID_WIDTH + col
                sample = samples[index]
                if not sample["land"]:
                    continue

                weighted_total = 0.0
                weight_sum = 0.0
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        neighbor_row = row + dy
                        neighbor_col = col + dx
                        if (
                            neighbor_row < 0
                            or neighbor_row >= GRID_HEIGHT
                            or neighbor_col < 0
                            or neighbor_col >= GRID_WIDTH
                        ):
                            continue

                        neighbor_index = neighbor_row * GRID_WIDTH + neighbor_col
                        if not samples[neighbor_index]["land"]:
                            continue

                        weight = 4.0 if dx == 0 and dy == 0 else 2.0 if dx == 0 or dy == 0 else 1.0
                        weighted_total += source[neighbor_index] * weight
                        weight_sum += weight

                if weight_sum > 0.0:
                    sample["elevation"] = weighted_total / weight_sum

    return samples


def svg_points(points, bounds, width, height, margin):
    result = []
    for x, y in points:
        nx, ny = normalized_xy(x, y, bounds)
        sx = margin + nx * (width - margin * 2)
        sy = margin + (1.0 - ny) * (height - margin * 2)
        result.append(f"{sx:.1f},{sy:.1f}")
    return " ".join(result)


def svg_normalized_points(points, width, height, margin):
    result = []
    for nx, ny in points:
        sx = margin + nx * (width - margin * 2)
        sy = margin + (1.0 - ny) * (height - margin * 2)
        result.append(f"{sx:.1f},{sy:.1f}")
    return " ".join(result)


def write_debug_artifacts(feature, outer, bounds, coastline, samples, landmarks, origin_lon, origin_lat):
    DEBUG_DIR.mkdir(parents=True, exist_ok=True)

    source_geojson = {
        "type": "FeatureCollection",
        "features": [feature],
    }
    (DEBUG_DIR / "oahu_source.geojson").write_text(
        json.dumps(source_geojson, indent=2),
        encoding="utf-8",
    )

    resampled_lonlat = []
    for nx, ny in coastline:
        x, y = denormalized_xy(nx, ny, bounds)
        resampled_lonlat.append(unproject(x, y, origin_lon, origin_lat))

    resampled_features = [
        {
            "type": "Feature",
            "properties": {"name": "resampled coastline", "points": len(resampled_lonlat)},
            "geometry": {
                "type": "LineString",
                "coordinates": resampled_lonlat + [resampled_lonlat[0]],
            },
        }
    ]
    for landmark in landmarks:
        resampled_features.append(
            {
                "type": "Feature",
                "properties": {
                    "name": landmark["name"],
                    "land": bool(landmark["land"]),
                    "normalized_x": landmark["x"],
                    "normalized_y": landmark["y"],
                },
                "geometry": {
                    "type": "Point",
                    "coordinates": [landmark["lon"], landmark["lat"]],
                },
            }
        )

    (DEBUG_DIR / "oahu_resampled.geojson").write_text(
        json.dumps({"type": "FeatureCollection", "features": resampled_features}, indent=2),
        encoding="utf-8",
    )

    width = 1000
    height = 760
    margin = 52
    source_points = svg_points(outer, bounds, width, height, margin)
    resampled_points = svg_normalized_points(coastline + [coastline[0]], width, height, margin)

    sample_dots = []
    for sample in samples:
        if not sample["land"]:
            continue
        sx = margin + sample["x"] * (width - margin * 2)
        sy = margin + (1.0 - sample["y"]) * (height - margin * 2)
        shade = 70 + min(170, int(sample["elevation"] / 1220.0 * 170.0))
        sample_dots.append(
            f'<circle cx="{sx:.1f}" cy="{sy:.1f}" r="2.2" fill="rgb(33,{shade},68)" />'
        )

    landmark_nodes = []
    for landmark in landmarks:
        sx = margin + landmark["x"] * (width - margin * 2)
        sy = margin + (1.0 - landmark["y"]) * (height - margin * 2)
        landmark_nodes.extend(
            [
                f'<circle cx="{sx:.1f}" cy="{sy:.1f}" r="5" fill="#ff4fd8" stroke="#111827" stroke-width="1.5" />',
                f'<text x="{sx + 8.0:.1f}" y="{sy - 8.0:.1f}" font-size="16" font-family="Segoe UI, Arial" fill="#111827">{landmark["name"]}</text>',
            ]
        )

    svg = "\n".join(
        [
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1000 760">',
            '<rect width="1000" height="760" fill="#75c7df" />',
            '<text x="52" y="34" font-size="22" font-family="Segoe UI, Arial" fill="#0f172a">Oahu topology debug preview: source, resampled coastline, grid, landmarks</text>',
            f'<polyline points="{source_points}" fill="none" stroke="#334155" stroke-width="3" opacity="0.55" />',
            "\n".join(sample_dots),
            f'<polyline points="{resampled_points}" fill="none" stroke="#ffe45e" stroke-width="4" />',
            "\n".join(landmark_nodes),
            '<text x="52" y="724" font-size="16" font-family="Segoe UI, Arial" fill="#0f172a">Gray: raw source ring. Yellow: resampled embedded ring. Green dots: terrain land samples.</text>',
            "</svg>",
            "",
        ]
    )
    (DEBUG_DIR / "oahu_preview.svg").write_text(svg, encoding="utf-8")

    metadata = {
        "source": "Hawaii Statewide GIS Program Coastline layer",
        "sourceFeature": feature.get("properties", {}),
        "sourcePointCount": len(outer),
        "resampledPointCount": len(coastline),
        "gridWidth": GRID_WIDTH,
        "gridHeight": GRID_HEIGHT,
        "elevationSmoothingPasses": ELEVATION_SMOOTHING_PASSES,
        "landSamples": sum(1 for sample in samples if sample["land"]),
        "landmarks": landmarks,
    }
    (DEBUG_DIR / "oahu_debug_metadata.json").write_text(
        json.dumps(metadata, indent=2),
        encoding="utf-8",
    )

    print(f"wrote debug artifacts under {DEBUG_DIR}")


def write_header(feature, lonlat_bounds, metric_bounds, coastline, samples, landmarks, source_point_count, hole_count):
    properties = feature.get("properties", {})
    max_elevation = max(sample["elevation"] for sample in samples)
    land_count = sum(1 for sample in samples if sample["land"])
    width_m = metric_bounds[2] - metric_bounds[0]
    height_m = metric_bounds[3] - metric_bounds[1]
    aspect = width_m / height_m
    sqmi = float(properties.get("sqmi", 0.0))

    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "// Generated by tools/fetch_oahu_topology.py.",
        "// Coastline source: Hawaii Statewide GIS Program Coastline layer,",
        "// derived from USGS Digital Line Graphs.",
        "// Elevation source: USGS National Map Elevation Point Query Service.",
        f"// Source feature: isle=Oahu, sqmi={sqmi:.3f}.",
        f"// Source coastline points: {source_point_count}. Interior rings: {hole_count}.",
        f"// Grid land samples: {land_count}.",
        f"// Elevation smoothing passes: {ELEVATION_SMOOTHING_PASSES}.",
        "",
        "struct OahuTopologyPoint {",
        "  float x;",
        "  float y;",
        "};",
        "",
        "struct OahuTerrainSample {",
        "  float x;",
        "  float y;",
        "  float elevationMeters;",
        "  std::uint8_t land;",
        "};",
        "",
        "struct OahuLandmark {",
        "  const char* name;",
        "  float longitude;",
        "  float latitude;",
        "  float x;",
        "  float y;",
        "  std::uint8_t land;",
        "};",
        "",
        f"constexpr int kOahuGridWidth = {GRID_WIDTH};",
        f"constexpr int kOahuGridHeight = {GRID_HEIGHT};",
        f"constexpr int kOahuCoastlinePointCount = {len(coastline)};",
        f"constexpr int kOahuLandmarkCount = {len(landmarks)};",
        f"constexpr int kOahuSourceCoastlinePointCount = {source_point_count};",
        f"constexpr int kOahuInteriorRingCount = {hole_count};",
        f"constexpr int kOahuElevationSmoothingPasses = {ELEVATION_SMOOTHING_PASSES};",
        f"constexpr float kOahuMinLongitude = {lonlat_bounds[0]:.7f}f;",
        f"constexpr float kOahuMinLatitude = {lonlat_bounds[1]:.7f}f;",
        f"constexpr float kOahuMaxLongitude = {lonlat_bounds[2]:.7f}f;",
        f"constexpr float kOahuMaxLatitude = {lonlat_bounds[3]:.7f}f;",
        f"constexpr float kOahuProjectedWidthMeters = {width_m:.3f}f;",
        f"constexpr float kOahuProjectedHeightMeters = {height_m:.3f}f;",
        f"constexpr float kOahuMapAspect = {aspect:.6f}f;",
        f"constexpr float kOahuSourceAreaSquareMiles = {sqmi:.3f}f;",
        f"constexpr float kOahuMaxElevationMeters = {max_elevation:.3f}f;",
        "",
        "inline constexpr std::array<OahuTopologyPoint, kOahuCoastlinePointCount> kOahuCoastline = {{",
    ]

    for x, y in coastline:
        lines.append(f"  {{ {x:.6f}f, {y:.6f}f }},")

    lines.extend(
        [
            "}};",
            "",
            "inline constexpr std::array<OahuLandmark, kOahuLandmarkCount> kOahuLandmarks = {{",
        ]
    )

    for landmark in landmarks:
        land = 1 if landmark["land"] else 0
        lines.append(
            f"  {{ \"{landmark['name']}\", {landmark['lon']:.7f}f, {landmark['lat']:.7f}f, "
            f"{landmark['x']:.6f}f, {landmark['y']:.6f}f, {land} }},"
        )

    lines.extend(
        [
            "}};",
            "",
            "inline constexpr std::array<OahuTerrainSample, kOahuGridWidth * kOahuGridHeight> kOahuTerrain = {{",
        ]
    )

    for sample in samples:
        land = 1 if sample["land"] else 0
        lines.append(
            f"  {{ {sample['x']:.6f}f, {sample['y']:.6f}f, {sample['elevation']:.3f}f, {land} }},"
        )

    lines.extend(["}};", ""])

    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {OUT}")


def main():
    feature, outer_lonlat, hole_lonlat = fetch_oahu_polygon()
    origin_lon, origin_lat = projection_origin([outer_lonlat] + hole_lonlat)
    outer = [project(lon, lat, origin_lon, origin_lat) for lon, lat in outer_lonlat]
    holes = [[project(lon, lat, origin_lon, origin_lat) for lon, lat in ring] for ring in hole_lonlat]

    lonlat_points = [point for point in outer_lonlat]
    lons = [point[0] for point in lonlat_points]
    lats = [point[1] for point in lonlat_points]
    lonlat_bounds = (min(lons), min(lats), max(lons), max(lats))

    bounds = projected_bounds(outer)
    coastline = sample_closed_ring(outer, COASTLINE_POINTS, bounds)
    landmarks = build_landmarks(bounds, origin_lon, origin_lat, outer, holes)
    samples = smooth_elevations(build_grid(outer, holes, bounds, origin_lon, origin_lat))
    write_debug_artifacts(feature, outer, bounds, coastline, samples, landmarks, origin_lon, origin_lat)
    write_header(
        feature,
        lonlat_bounds,
        bounds,
        coastline,
        samples,
        landmarks,
        len(outer_lonlat),
        len(holes),
    )


if __name__ == "__main__":
    main()
