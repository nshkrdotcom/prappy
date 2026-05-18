# Setup

This guide is written for the local repo layout:

```text
C:\Users\windo\projects\n\prappy
C:\Users\windo\projects\dotfiles_private
```

The Prappy repo contains the app. The dotfiles repo contains the reusable
Windows-native C++ setup scripts.

## First-Time Setup

Open PowerShell 7 or Windows Terminal:

```powershell
cd C:\Users\windo\projects\n\prappy
pwsh scripts\bootstrap.ps1
```

If the bootstrap installs or updates PowerShell, Git, CMake, Ninja, or Visual
Studio Build Tools, close and reopen PowerShell before continuing.

Then run:

```powershell
pwsh scripts\doctor.ps1
pwsh scripts\deps.ps1
pwsh scripts\configure.ps1
pwsh scripts\build.ps1
pwsh scripts\run.ps1
```

## What Each Script Does

- `bootstrap.ps1` installs or verifies the base Windows tooling.
- `doctor.ps1` checks that the required tools are visible to the scripts.
- `deps.ps1` clones or updates SDL3, bgfx, bx, bimg, and Dear ImGui in `external\`.
- `configure.ps1` prepares the CMake build directory.
- `build.ps1` compiles the app and shaders.
- `run.ps1` starts the app.

You do not need to open "Developer PowerShell for Visual Studio". The shared
tooling imports the MSVC environment for you.

## Expected Outputs

Release builds go here:

```text
build\windows-msvc-release\prappy_native.exe
```

Debug builds go here:

```text
build\windows-msvc-debug\prappy_native.exe
```

Dependencies are cloned under:

```text
external\
```

`external\` and `build\` are generated directories and are intentionally not
tracked by git.
