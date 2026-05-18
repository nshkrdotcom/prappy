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

- Coastline geometry from OpenStreetMap through Nominatim.
- Elevation samples from the USGS National Map Elevation Point Query Service.

## When To Regenerate

Regenerate when you want to change:

- Grid density.
- Bounding box.
- Elevation sampling behavior.
- Coastline simplification.
- Terrain coloring or derived metadata.

After regenerating, build and smoke-test Oahu:

```powershell
pwsh scripts\build.ps1
pwsh scripts\run.ps1 -Visualization Oahu -SmokeTest -ScreenshotSmoke
```

## Practical Notes

Keep generated data deterministic. The app should not require a network call to
launch, render, or smoke-test.

If the external data services change behavior, fix the generator and commit the
new generated `src\oahu_topology.h` together with the generator change.
