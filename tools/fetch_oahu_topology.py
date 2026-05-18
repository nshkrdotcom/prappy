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
USER_AGENT = "prappy-native-oahu-data/1.0 (local development)"
GRID_WIDTH = 37
GRID_HEIGHT = 27
COASTLINE_POINTS = 192


def fetch_json(url: str) -> dict:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.loads(response.read().decode("utf-8"))


def fetch_oahu_polygon():
    query = urllib.parse.urlencode(
        {
            "q": "Oahu, Hawaii",
            "format": "jsonv2",
            "polygon_geojson": "1",
            "limit": "1",
        }
    )
    results = fetch_json(f"https://nominatim.openstreetmap.org/search?{query}")
    if not results:
        raise RuntimeError("Nominatim returned no Oahu result")

    result = results[0]
    geojson = result["geojson"]
    if geojson["type"] != "Polygon":
        raise RuntimeError(f"Expected Polygon, got {geojson['type']}")

    return result, geojson["coordinates"][0]


def point_in_polygon(lon: float, lat: float, polygon) -> bool:
    inside = False
    j = len(polygon) - 1
    for i, point in enumerate(polygon):
        xi, yi = point
        xj, yj = polygon[j]
        crosses = (yi > lat) != (yj > lat)
        if crosses:
            x_at_y = (xj - xi) * (lat - yi) / ((yj - yi) or 1.0e-12) + xi
            if lon < x_at_y:
                inside = not inside
        j = i
    return inside


def normalized_xy(lon: float, lat: float, bounds):
    min_lon, min_lat, max_lon, max_lat = bounds
    x = (lon - min_lon) / (max_lon - min_lon)
    y = (lat - min_lat) / (max_lat - min_lat)
    return x, y


def sample_closed_ring(points, count, bounds):
    normalized = [normalized_xy(lon, lat, bounds) for lon, lat in points]
    if normalized[0] != normalized[-1]:
        normalized.append(normalized[0])

    distances = [0.0]
    total = 0.0
    for a, b in zip(normalized, normalized[1:]):
        total += math.dist(a, b)
        distances.append(total)

    sampled = []
    segment = 0
    for i in range(count):
        target = total * i / count
        while segment + 1 < len(distances) and distances[segment + 1] < target:
            segment += 1

        start = normalized[segment]
        end = normalized[(segment + 1) % len(normalized)]
        segment_length = distances[segment + 1] - distances[segment]
        t = 0.0 if segment_length <= 0.0 else (target - distances[segment]) / segment_length
        sampled.append((start[0] + (end[0] - start[0]) * t, start[1] + (end[1] - start[1]) * t))

    return sampled


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
    for attempt in range(3):
        try:
            data = fetch_json(url)
            value = data.get("value")
            if value is None:
                return 0.0
            return max(float(value), 0.0)
        except Exception as exc:
            last_error = exc
            time.sleep(0.25 * (attempt + 1))

    print(f"warning: elevation lookup failed for {lon},{lat}: {last_error}", file=sys.stderr)
    return 0.0


def build_grid(polygon, bounds):
    samples = []
    jobs = {}
    with ThreadPoolExecutor(max_workers=6) as executor:
      for row in range(GRID_HEIGHT):
          for col in range(GRID_WIDTH):
              lon = bounds[0] + (bounds[2] - bounds[0]) * col / (GRID_WIDTH - 1)
              lat = bounds[1] + (bounds[3] - bounds[1]) * row / (GRID_HEIGHT - 1)
              land = point_in_polygon(lon, lat, polygon)
              sample = {
                  "x": col / (GRID_WIDTH - 1),
                  "y": row / (GRID_HEIGHT - 1),
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
          if completed % 50 == 0 or completed == total:
              print(f"elevation samples: {completed}/{total}")

    return samples


def write_header(result, bounds, coastline, samples):
    max_elevation = max(sample["elevation"] for sample in samples)
    land_count = sum(1 for sample in samples if sample["land"])
    osm_id = result.get("osm_id", "")

    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "// Generated by tools/fetch_oahu_topology.py.",
        "// Coastline source: OpenStreetMap/Nominatim relation for Oahu.",
        "// Elevation source: USGS National Map Elevation Point Query Service.",
        f"// OSM relation id: {osm_id}. Grid land samples: {land_count}.",
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
        f"constexpr int kOahuGridWidth = {GRID_WIDTH};",
        f"constexpr int kOahuGridHeight = {GRID_HEIGHT};",
        f"constexpr int kOahuCoastlinePointCount = {len(coastline)};",
        f"constexpr float kOahuMinLongitude = {bounds[0]:.7f}f;",
        f"constexpr float kOahuMinLatitude = {bounds[1]:.7f}f;",
        f"constexpr float kOahuMaxLongitude = {bounds[2]:.7f}f;",
        f"constexpr float kOahuMaxLatitude = {bounds[3]:.7f}f;",
        f"constexpr float kOahuMaxElevationMeters = {max_elevation:.3f}f;",
        "",
        "inline constexpr std::array<OahuTopologyPoint, kOahuCoastlinePointCount> kOahuCoastline = {{",
    ]

    for x, y in coastline:
        lines.append(f"  {{ {x:.6f}f, {y:.6f}f }},")

    lines.extend([
        "}};",
        "",
        "inline constexpr std::array<OahuTerrainSample, kOahuGridWidth * kOahuGridHeight> kOahuTerrain = {{",
    ])

    for sample in samples:
        land = 1 if sample["land"] else 0
        lines.append(
            f"  {{ {sample['x']:.6f}f, {sample['y']:.6f}f, {sample['elevation']:.3f}f, {land} }},"
        )

    lines.extend([
        "}};",
        "",
    ])

    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {OUT}")


def main():
    result, polygon = fetch_oahu_polygon()
    lons = [point[0] for point in polygon]
    lats = [point[1] for point in polygon]
    lon_pad = (max(lons) - min(lons)) * 0.08
    lat_pad = (max(lats) - min(lats)) * 0.10
    bounds = (
        min(lons) - lon_pad,
        min(lats) - lat_pad,
        max(lons) + lon_pad,
        max(lats) + lat_pad,
    )

    coastline = sample_closed_ring(polygon, COASTLINE_POINTS, bounds)
    samples = build_grid(polygon, bounds)
    write_header(result, bounds, coastline, samples)


if __name__ == "__main__":
    main()
