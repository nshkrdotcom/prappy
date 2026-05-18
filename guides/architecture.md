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

The current source split is:

```text
src\app.*
src\renderer.*
src\visualization_core.*
src\visualizations\random_lines.*
src\visualizations\starfield.*
src\visualizations\oahu_flyover.*
src\visualizations\particle_field.*
src\platform\screenshot.*
```

## Renderer Layer

Visualizations submit custom bgfx vertex buffers. Current primitives include:

- Screen-space line lists.
- 3D line lists.
- Particle streak line lists.
- Terrain triangles.
- Coastline and diagnostic line overlays.

The renderer abstraction is intentionally bgfx-level, not CUDA-level. That keeps
the app portable across graphics APIs while still giving access to GPU buffers,
shaders, and compute-capable backends later.

The renderer can be selected at launch:

```powershell
pwsh scripts\run.ps1 -Renderer D3D11
pwsh scripts\run.ps1 -Renderer D3D12
pwsh scripts\run.ps1 -Renderer Vulkan
```

The diagnostics panels show the selected backend, requested backend, supported
backends, vendor/device IDs, and key capabilities such as compute, instancing,
texture readback, and indirect draw support.

## Screenshot Path

Screenshots use bgfx's framebuffer screenshot callback rather than a desktop
screen grab. That makes capture reproducible even if another window overlaps
the app. The callback crops the framebuffer to the current visualization canvas
and writes a BMP file under `captures\`.

## Regression Coverage

`scripts\visual-regression.ps1` is the first behavior-level test layer. It
validates the generated Oahu topology header, runs all visualizations, captures
screenshots, verifies BMP dimensions, checks for nonblank pixels, and
smoke-tests a D3D11 renderer override.
