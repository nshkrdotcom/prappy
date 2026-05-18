# Oahu Topology Data

The Oahu flyover uses generated topology data in:

```text
src\oahu_topology.h
```

That header stores coastline points and sampled elevation data in a static C++
data structure so the app can run without fetching data at startup.

## Refresh The Data

From the repo root:

```powershell
python tools\fetch_oahu_topology.py
```

The generator pulls:

- Coastline geometry from the Hawaii Statewide GIS Program `Coastline` layer,
  filtered to the main Oahu polygon.
- Elevation samples from the USGS National Map Elevation Point Query Service.

The coastline layer is exposed by the State GIS ArcGIS service as GeoJSON and
is derived from USGS Digital Line Graphs. The generator keeps the source ring in
projected meters before normalizing it, so the rendered island preserves the
east-west/north-south aspect instead of stretching raw longitude and latitude.

The generator also writes debug artifacts under:

```text
build\oahu_debug\
```

Those files are intentionally generated output:

- `oahu_source.geojson`: the raw selected Hawaii GIS source feature.
- `oahu_resampled.geojson`: the exact resampled coastline used by the embedded C++ header plus landmark points.
- `oahu_preview.svg`: a labeled top-down preview with source ring, resampled ring, terrain samples, and landmarks.
- `oahu_debug_metadata.json`: source and derived metrics.

## Validate The Generated Header

After regenerating, run:

```powershell
python tools\validate_oahu_topology.py --require-debug-artifacts
```

That check verifies the source, sample count, Oahu bounds, aspect ratio, source
area, grid density, and sampled elevation range.

## When To Regenerate

Regenerate when you want to change:

- Grid density.
- Bounding box.
- Elevation sampling behavior.
- Coastline simplification.
- Terrain coloring or derived metadata.

After regenerating, build and smoke-test Oahu:

```powershell
python tools\validate_oahu_topology.py --require-debug-artifacts
pwsh scripts\build.ps1
pwsh scripts\run.ps1 -Visualization Oahu -SmokeTest -ScreenshotSmoke
```

## Isolate Shape Problems

Use the app's top-down diagnostic mode to separate the problem layer:

```powershell
pwsh scripts\run.ps1 -Visualization Oahu -OahuDiagnostic Coastline -Focus -NoOverlay
pwsh scripts\run.ps1 -Visualization Oahu -OahuDiagnostic Mesh -Focus -NoOverlay
pwsh scripts\run.ps1 -Visualization Oahu -OahuDiagnostic All -Focus -NoOverlay
```

Interpretation:

- If `Coastline` looks wrong, inspect `build\oahu_debug\oahu_source.geojson` and `build\oahu_debug\oahu_resampled.geojson`.
- If `Coastline` looks right but `Mesh` looks wrong, the terrain grid/fill rule is the problem.
- If top-down looks right but the normal flyover looks wrong, the issue is camera perspective, route, elevation scale, or overlay occlusion.

## Practical Notes

Keep generated data deterministic. The app should not require a network call to
launch, render, or smoke-test.

If the external data services change behavior, fix the generator and commit the
new generated `src\oahu_topology.h` together with the generator change.
