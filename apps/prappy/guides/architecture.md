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
- `VisualizationPresetDescriptor`: named presentation states for demos, captures, and repeatable debugging.
- `IVisualizationModule`: reset, draw, and inspector contract.
- `VisualizationHost`: owns modules, active selection, reset lifecycle, and camera state.
- `VisualizationContext`: per-frame rendering context passed into modules.
- `CameraRig`: reusable camera state for 3D modules.

This gives the app a stable place to add new visualizations without growing
more UI-specific branching.

The current source split is:

```text
src\app.*
src\renderer.* (color submission + resource helpers)
src\visualization_core.*
src\visualizations\random_lines.*
src\visualizations\starfield.*
src\visualizations\oahu_flyover.*
src\visualizations\particle_field.*
src\platform\screenshot.*
```

## Renderer Layer

Visualizations submit custom bgfx vertex buffers and shaders. Current primitives
include:

- Screen-space line lists.
- 3D line lists.
- Compute-updated particle buffers with CPU simulation fallback.
- Retained indexed Oahu terrain mesh.
- Coastline and diagnostic line overlays.

The renderer abstraction is intentionally bgfx-level, not CUDA-level. That keeps
the app portable across graphics APIs while still giving access to GPU buffers,
shaders, and compute-capable backends later.

The retained paths share small resource helpers:

- `ShaderProgram` owns graphics/compute program load and shutdown.
- `DynamicVertexBuffer` owns capacity, stride, flags, update bytes, and shutdown.
- `RenderPassDiagnostics` exposes pass name, shader, backend, draw/dispatch
  counts, vertices, upload bytes, and compute state to the UI.

The particle field uses bgfx compute when `BGFX_CAPS_COMPUTE` is available:
particle state is updated in `particle_update_cs`, then the compute-written
dynamic vertex buffer is submitted through the particle draw shader. If compute
is unavailable, the same visualization falls back to CPU simulation and uploads
particle vertices into the same retained draw path. CUDA is not involved.

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
screenshots, verifies BMP dimensions, checks for nonblank pixels, captures a
top-down Oahu diagnostic layer view, validates exact-size presentation captures,
and smoke-tests a D3D11 renderer override.

Presentation presets can also be smoke-tested directly, for example:

```powershell
pwsh scripts\run.ps1 -Preset OahuCenteredTopDown -SmokeTest -ScreenshotSmoke
```

Presentation mode is runtime-only: scripts pass fixed timestep, window size,
profile, capture path, capture frame, and exit frame arguments to the normal
executable. That keeps captures reproducible without adding a gallery or a
second rendering entry point.

## Oahu Data Isolation

The Oahu topology generator writes both committed C++ data and ignored debug
artifacts. The committed header keeps runtime startup network-free; the debug
artifacts let the source coastline, resampled coastline, terrain grid, and
landmark controls be inspected separately from bgfx rendering and the flyover
camera.
