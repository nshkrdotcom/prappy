# Architecture

Prappy is intentionally small, native, and explicit. The stack is:

- SDL3: window creation, platform handles, events, input.
- bgfx: renderer abstraction over Direct3D, Vulkan, OpenGL, and other backends.
- Dear ImGui: immediate-mode tool UI.
- CMake and Ninja: build graph and build executor.
- MSVC: native Windows compiler and linker.

## Runtime Shape

The app has two main render surfaces:

- A bgfx visualization view for GPU-drawn content.
- An ImGui UI pass for menus, panels, diagnostics, and tool controls.

The visualization canvas is measured from the ImGui layout, then bgfx renders
into the corresponding viewport rectangle.

## Visualization Layer

The core internal types are:

- `VisualizationDescriptor`: metadata consumed by the menu, command bar, inspector, and diagnostics.
- `IVisualizationModule`: reset, draw, and inspector contract.
- `VisualizationHost`: owns modules, active selection, reset lifecycle, and camera state.
- `VisualizationContext`: per-frame rendering context passed into modules.
- `CameraRig`: reusable camera state for 3D modules.

This gives the app a stable place to add new visualizations without growing
more UI-specific branching.

## Renderer Layer

Visualizations submit custom bgfx vertex buffers. Current primitives include:

- Screen-space line lists.
- 3D line lists.
- Terrain triangles.
- Coastline and diagnostic line overlays.

The renderer abstraction is intentionally bgfx-level, not CUDA-level. That keeps
the app portable across graphics APIs while still giving access to GPU buffers,
shaders, and compute-capable backends later.

## Screenshot Path

Screenshots use bgfx's framebuffer screenshot callback rather than a desktop
screen grab. That makes capture reproducible even if another window overlaps
the app. The callback crops the framebuffer to the current visualization canvas
and writes a BMP file under `captures\`.

## Future Split

The code is still compact enough to live mostly in `src\main.cpp`, but the
stable next split is:

```text
src\app.*
src\renderer.*
src\visualization_core.*
src\visualizations\random_lines.*
src\visualizations\starfield.*
src\visualizations\oahu_flyover.*
src\platform\screenshot.*
```

That split should happen when the next visualization or renderer pass makes the
single-file layout slow to navigate.
