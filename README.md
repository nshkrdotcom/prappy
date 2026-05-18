# Prappy

Prappy is the sample native Windows app for the setup in
`C:\Users\windo\projects\dotfiles_private`.

The app is C++20 with SDL3, bgfx, Dear ImGui, CMake, Ninja, and MSVC. The app
code lives here. The reusable Windows setup and build tooling lives in:

```text
C:\Users\windo\projects\dotfiles_private\windows\bootstrap_windows\native_cpp
```

The scripts in this repo are intentionally small wrappers around that shared
tooling. That means the same commands used here should work again on a clean
Windows 11 machine after cloning the dotfiles repo and this repo into the same
layout.

## Where To Run Commands

Open PowerShell 7 or Windows Terminal, then go to this repo:

```powershell
cd C:\Users\windo\projects\n\prappy
```

You do not need to open "Developer PowerShell for Visual Studio". The scripts
find the Visual Studio Build Tools environment themselves.

## First-Time Setup

Run these in order from `C:\Users\windo\projects\n\prappy`:

```powershell
pwsh scripts\bootstrap.ps1
pwsh scripts\doctor.ps1
pwsh scripts\deps.ps1
pwsh scripts\configure.ps1
pwsh scripts\build.ps1
pwsh scripts\run.ps1
```

What these do:

- `bootstrap.ps1` installs or verifies PowerShell, Git, CMake, Ninja, and MSVC Build Tools through the dotfiles setup.
- `doctor.ps1` checks that the required tools are visible to the scripts.
- `deps.ps1` downloads SDL3, bgfx, bx, bimg, and Dear ImGui into `external\`.
- `configure.ps1` prepares the CMake build directory.
- `build.ps1` compiles the app and shaders.
- `run.ps1` starts the app.

If `bootstrap.ps1` installs or updates tooling, close and reopen PowerShell
before continuing with the later steps.

## Daily Development Loop

Most of the time, edit files under `src\`, then run:

```powershell
pwsh scripts\build.ps1
pwsh scripts\run.ps1
```

For a quick non-interactive check that opens, renders a few frames, and exits:

```powershell
pwsh scripts\run.ps1 -SmokeTest
```

Launch a specific visualization from the script:

```powershell
pwsh scripts\run.ps1 -Visualization RandomLines
pwsh scripts\run.ps1 -Visualization Starfield
```

The smoke test writes its log under the active build directory, for example:

```text
build\windows-msvc-release\prappy_smoke.log
```

## Debug Build

Use Debug when you want debug symbols and assertions:

```powershell
pwsh scripts\configure.ps1 -Config Debug
pwsh scripts\build.ps1 -Config Debug
pwsh scripts\run.ps1 -Config Debug
```

Debug smoke test:

```powershell
pwsh scripts\run.ps1 -Config Debug -SmokeTest
```

## Release Build

Release is the default, so these are equivalent:

```powershell
pwsh scripts\build.ps1
pwsh scripts\build.ps1 -Config Release
```

The release executable is built at:

```text
build\windows-msvc-release\prappy_native.exe
```

## Clean And Rebuild

To remove generated build files:

```powershell
pwsh scripts\clean.ps1
```

Then rebuild from scratch:

```powershell
pwsh scripts\configure.ps1
pwsh scripts\build.ps1
```

`clean.ps1` removes `build\`. It does not remove `external\`, so dependencies
do not need to be downloaded again.

## Refresh Dependencies

Dependencies are declared in `native-deps.json`.

Run this to clone or update them:

```powershell
pwsh scripts\deps.ps1
```

After a successful dependency sync, the exact commits are written to
`native-deps.lock.json`. Commit that lock file when dependency versions change.

`external\` is intentionally ignored by git.

## Regenerate From Dotfiles

The dotfiles repo can regenerate this sample app layout:

```powershell
cd C:\Users\windo\projects\dotfiles_private
pwsh windows\bootstrap_windows\Initialize-PrappyNativeSample.ps1 -Force
```

The full setup/build/smoke-test path from dotfiles is:

```powershell
cd C:\Users\windo\projects\dotfiles_private
pwsh windows\bootstrap_windows\Invoke-PrappyNativeSample.ps1 All -Force -SmokeTest
```

Use `-Force` only when you intentionally want the generated template files in
this repo refreshed from dotfiles.

## Troubleshooting

If a script cannot find the dotfiles tooling, run:

```powershell
$env:DOTFILES_PRIVATE = "C:\Users\windo\projects\dotfiles_private"
```

Then rerun the failed command from the same PowerShell window.

If CMake, Ninja, or MSVC are missing, rerun:

```powershell
pwsh scripts\bootstrap.ps1
```

If the app was previously configured before dependency or toolchain changes,
clear the build directory and configure again:

```powershell
pwsh scripts\clean.ps1
pwsh scripts\configure.ps1
pwsh scripts\build.ps1
```
