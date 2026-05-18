# Visualization Guide

Prappy currently ships three visualizations:

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

## Controls

The 3D visualizations share a camera rig:

- Left drag: orbit/look.
- Right drag: pan.
- Mouse wheel: zoom for terrain, FOV adjustment for the starfield.
- Reset Camera: restore the visualization's default camera state.

The Oahu flyover also has an auto-route mode. Disable auto-route from the
inspector or by interacting with the canvas.

The particle field exposes inspector controls for particle count, speed, spread,
turbulence, trail length, hue drift, and reset. It uses bgfx-submitted vertex
buffers for particle streaks, so it is the first visualization that is explicitly
structured as a reusable GPU draw workload.

## Module Contract

Each visualization provides:

- Descriptor metadata: name, short label, coordinate space, primitive type, and camera support.
- Reset behavior.
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
  void drawInspector() const override;
};
```

Then register it in `VisualizationHost`.

Visualization modules now live under `src\visualizations\`, with shared
contracts in `src\visualization_core.*`.
