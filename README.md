<p align="center">
  <img src="assets/prappy.svg" alt="Prappy - Native GPU Visualization Engine" width="900"/>
</p>

<p align="center">
  <a href="https://github.com/nshkrdotcom/frappy">
    <img alt="Related repo: nshkrdotcom/frappy" src="https://img.shields.io/badge/github-nshkrdotcom%2Ffrappy-181717?logo=github"/>
  </a>
  <a href="LICENSE">
    <img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-blue.svg"/>
  </a>
</p>

# Prappy

Prappy is a native C++20 GPU visualization workbench. It uses SDL3 for the
window and input layer, bgfx for renderer abstraction, Dear ImGui for the tool
UI, and CMake/Ninja/MSVC for the Windows build.

The app lives in:

```text
C:\Users\windo\projects\n\prappy
```

The reusable Windows setup and native C++ tooling live in:

```text
C:\Users\windo\projects\dotfiles_private\windows\bootstrap_windows\native_cpp
```

The scripts in this repo are thin wrappers around that shared tooling, so the
same commands should work on a clean Windows 11 machine after cloning the repos
into the expected layout.

## Quick Start

Open PowerShell 7 or Windows Terminal:

```powershell
cd C:\Users\windo\projects\n\prappy
pwsh scripts\bootstrap.ps1
pwsh scripts\doctor.ps1
pwsh scripts\deps.ps1
pwsh scripts\configure.ps1
pwsh scripts\build.ps1
pwsh scripts\run.ps1
```

If `bootstrap.ps1` installs or updates tools, close and reopen PowerShell before
continuing with the later commands.

## Daily Loop

```powershell
cd C:\Users\windo\projects\n\prappy
pwsh scripts\build.ps1
pwsh scripts\run.ps1
```

Smoke-test the current build:

```powershell
pwsh scripts\run.ps1 -Visualization Oahu -SmokeTest -ScreenshotSmoke
```

Screenshots from the capture path are written below the active build directory:

```text
build\windows-msvc-release\captures\
```

## Guides

- [Setup](guides/setup.md)
- [Development Workflow](guides/development.md)
- [Visualization Guide](guides/visualizations.md)
- [Architecture](guides/architecture.md)
- [Oahu Topology Data](guides/oahu-topology.md)
- [Reproducible Windows Tooling](guides/windows-tooling.md)
- [Troubleshooting](guides/troubleshooting.md)

## Current Visualizations

- Random Lines 2D
- Infinite Starfield
- Oahu Flyover

The app has a reusable visualization module contract, shared camera controls for
3D modules, renderer diagnostics, and a bgfx-backed screenshot export path.
