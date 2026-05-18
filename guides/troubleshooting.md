# Troubleshooting

## Script Cannot Find Dotfiles Tooling

Set `DOTFILES_PRIVATE` for the current PowerShell session:

```powershell
$env:DOTFILES_PRIVATE = "C:\Users\windo\projects\dotfiles_private"
```

Then rerun the command.

## CMake, Ninja, Git, Or MSVC Is Missing

Run:

```powershell
pwsh scripts\bootstrap.ps1
```

If anything was installed or updated, close and reopen PowerShell before
continuing.

## Build Fails After Dependency Changes

Clear the build directory and configure again:

```powershell
pwsh scripts\clean.ps1
pwsh scripts\configure.ps1
pwsh scripts\build.ps1
```

## Dependencies Are Missing

Run:

```powershell
pwsh scripts\deps.ps1
```

Dependencies should appear under:

```text
external\
```

## App Opens But A Visualization Looks Wrong

Run the targeted smoke test:

```powershell
pwsh scripts\run.ps1 -Visualization Oahu -SmokeTest -ScreenshotSmoke
```

Then inspect:

```text
build\windows-msvc-release\prappy_smoke.log
build\windows-msvc-release\captures\
```

## Screenshot Is Missing

Use the scripted capture path:

```powershell
pwsh scripts\run.ps1 -Visualization Oahu -SmokeTest -ScreenshotSmoke
```

Screenshots are produced by the bgfx callback after a frame is submitted, so the
app needs to reach the render loop successfully.

## Debug Output Is Too Noisy

Use Release for normal work:

```powershell
pwsh scripts\build.ps1 -Config Release
pwsh scripts\run.ps1 -Config Release
```
