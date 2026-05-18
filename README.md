# Prappy

Prappy currently contains a Windows-native C++20 sample app using SDL3, bgfx,
Dear ImGui, CMake, Ninja, and MSVC.

The reusable Windows toolchain and build orchestration live in:

```text
dotfiles_private/windows/bootstrap_windows/native_cpp
```

The scripts in this repo are thin wrappers that delegate there, so the same
setup is reproducible from the dotfiles Windows bootstrap on a clean Windows 11
machine.

## Setup

Open PowerShell 7 from this repository root.

```powershell
pwsh scripts/bootstrap.ps1
pwsh scripts/deps.ps1
pwsh scripts/configure.ps1
pwsh scripts/build.ps1
pwsh scripts/run.ps1
```

For a non-interactive smoke test:

```powershell
pwsh scripts/run.ps1 -SmokeTest
```

## Debug Build

```powershell
pwsh scripts/configure.ps1 -Config Debug
pwsh scripts/build.ps1 -Config Debug
pwsh scripts/run.ps1 -Config Debug
```

## Clean

```powershell
pwsh scripts/clean.ps1
```

## Dependency Lock

`native-deps.json` declares source dependencies. `scripts/deps.ps1` writes
`native-deps.lock.json` with exact commits after a successful dependency sync.

