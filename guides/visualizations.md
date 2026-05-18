# Visualization Guide

Prappy currently ships four visualizations:

- Random Lines 2D
- Infinite Starfield
- Oahu Flyover
- GPU Particle Field

Each visualization is exposed through a reusable module contract rather than a
large switch in the UI.

## Launch A Visualization

```powershell
pwsh scripts\run.ps1 -Visualization RandomLines
pwsh scripts\run.ps1 -Visualization Starfield
pwsh scripts\run.ps1 -Visualization Oahu
pwsh scripts\run.ps1 -Visualization ParticleField
```

The same options work with `-SmokeTest`.

## Presentation Presets

Presets apply the visualization, camera/view state, and any visualization-specific
presentation settings needed for a stable demo or capture:

```powershell
pwsh scripts\run.ps1 -Preset RandomLinesHero
pwsh scripts\run.ps1 -Preset StarfieldHero
pwsh scripts\run.ps1 -Preset OahuFlyover
pwsh scripts\run.ps1 -Preset OahuCenteredTopDown
pwsh scripts\run.ps1 -Preset OahuDebugMesh
pwsh scripts\run.ps1 -Preset ParticlesHero
```

The same presets are available from the `Visualization > Preset` menu and the
inspector.

## Controls

The 3D visualizations share a camera rig:

- Left drag: orbit/look.
- Right drag: pan.
- Mouse wheel: zoom for terrain, FOV adjustment for the starfield.
- Reset Camera: restore the visualization's default camera state.

The Oahu flyover uses a generated USGS/Hawaii GIS coastline and elevation data
header. It also has an auto-route mode. Disable auto-route from the inspector or
by interacting with the canvas.

When Oahu is active, the normal command bar and the `Oahu` menu expose named
`Flyover`, `Centered Top Down`, and `Debug Mesh` presentation presets plus
diagnostic layer toggles. In Focus mode, those same controls appear inside the
visualization canvas unless `-NoOverlay` is used.

Oahu has an isolation mode for map-shape debugging:

```powershell
pwsh scripts\run.ps1 -Visualization Oahu -OahuDiagnostic Coastline -Focus -NoOverlay
pwsh scripts\run.ps1 -Preset OahuDebugMesh -Focus -NoOverlay
pwsh scripts\run.ps1 -Preset OahuCenteredTopDown -Focus -NoOverlay
```

The visual inspector exposes the same layer toggles for background, filled
terrain, coastline, terrain grid, ridge lines, landmarks, and top-down view.
The generated Oahu data is intentionally high-density: 4,096 coastline samples,
a 241 x 181 terrain grid, and smoothed USGS elevation samples.

Oahu filled terrain is a retained bgfx indexed mesh with a dedicated terrain
shader. Coastline, ridge, grid, and landmark diagnostics remain transient line
overlays so they can be toggled without rebuilding the terrain buffers.

The particle field exposes inspector controls for particle count, speed, spread,
turbulence, trail length, hue drift, and reset. It uses bgfx-submitted vertex
buffers for particle streaks, so it is the first visualization that is explicitly
structured as a reusable GPU draw workload.

## Module Contract

Each visualization provides:

- Descriptor metadata: name, short label, coordinate space, primitive type, and camera support.
- Named presentation presets.
- Reset behavior.
- Resource shutdown behavior.
- Draw behavior.
- Inspector UI.

This keeps new visualizations from needing custom wiring throughout the menu,
toolbar, diagnostics, and inspector panels.

## Adding The Next Visualization

Add a module that implements the same shape as the existing visualizations:

```cpp
struct MyVisualization final : IVisualizationModule {
  const VisualizationDescriptor& descriptor() const override;
  void reset(const ImVec2& size) override;
  void draw(VisualizationContext& context) override;
  void drawInspector() override;
};
```

Then register it in `VisualizationHost`.

Visualization modules now live under `src\visualizations\`, with shared
contracts in `src\visualization_core.*`.
