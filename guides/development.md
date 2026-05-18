# Development Workflow

Most work happens in `src\main.cpp`, `src\oahu_topology.h`, shader files under
`src\shaders\`, and scripts under `scripts\`.

## Build And Run

```powershell
cd C:\Users\windo\projects\n\prappy
pwsh scripts\build.ps1
pwsh scripts\run.ps1
```

Release is the default configuration.

## Debug Build

```powershell
pwsh scripts\configure.ps1 -Config Debug
pwsh scripts\build.ps1 -Config Debug
pwsh scripts\run.ps1 -Config Debug
```

Use Debug when you want debug symbols, assertions, and easier native debugging.

## Smoke Tests

Smoke tests open the app, render a few frames, and exit.

```powershell
pwsh scripts\run.ps1 -SmokeTest
pwsh scripts\run.ps1 -Visualization RandomLines -SmokeTest
pwsh scripts\run.ps1 -Visualization Starfield -SmokeTest
pwsh scripts\run.ps1 -Visualization Oahu -SmokeTest
```

The smoke log is written beside the executable, for example:

```text
build\windows-msvc-release\prappy_smoke.log
```

## Screenshot Smoke Test

Use this when changing render views, layout, diagnostics, or screenshot code:

```powershell
pwsh scripts\run.ps1 -Visualization Oahu -SmokeTest -ScreenshotSmoke
```

The capture is written under:

```text
build\windows-msvc-release\captures\
```

## Clean Build

```powershell
pwsh scripts\clean.ps1
pwsh scripts\configure.ps1
pwsh scripts\build.ps1
```

`clean.ps1` removes `build\`. It does not remove `external\`, so dependencies
do not need to be downloaded again.

## Dependency Refresh

Dependencies are declared in `native-deps.json`.

```powershell
pwsh scripts\deps.ps1
```

After a dependency sync, exact commits are written to `native-deps.lock.json`.
Commit that lock file when dependency versions intentionally change.
