# Reproducible Windows Tooling

Prappy is the sample app. The reusable setup lives in:

```text
C:\Users\Home\projects\dotfiles_private\windows\bootstrap_windows\native_cpp
```

The scripts in this repo call into that shared tooling through
`scripts\_tooling.ps1`.

## Why The Wrapper Scripts Are Small

The app repo should stay focused on app code. The Windows environment setup,
toolchain discovery, dependency handling, and template generation belong in the
dotfiles repo so they can be reused on a clean Windows 11 machine.

## Template Generation

The dotfiles generator is for new sample checkouts. This app now lives in the
Prappy monorepo at `apps/prappy`; use its local wrapper scripts for normal work.
Regenerating this app from an older template would replace its source and tooling.
For a separate disposable sample, pass an explicit destination:

```powershell
cd C:\Users\Home\projects\dotfiles_private
pwsh windows\bootstrap_windows\Initialize-PrappyNativeSample.ps1 -ProjectRoot "$env:TEMP\prappy-sample"
```

Do not use template generation as an update command for this monorepo.

## Full Dotfiles Setup Path

```powershell
cd C:\Users\Home\projects\dotfiles_private
pwsh windows\bootstrap_windows\Invoke-PrappyNativeSample.ps1 Doctor -ProjectRoot C:\Users\Home\projects\prappy\apps\prappy
```

For screenshot verification through the wrapper:

```powershell
pwsh windows\bootstrap_windows\Invoke-PrappyNativeSample.ps1 Run -ProjectRoot C:\Users\Home\projects\prappy\apps\prappy -Visualization ParticleField -SmokeTest -ScreenshotSmoke -Renderer D3D11
```

## If The Tooling Cannot Be Found

Set this in the current PowerShell session:

```powershell
$env:DOTFILES_PRIVATE = "C:\Users\Home\projects\dotfiles_private"
```

Then rerun the failed Prappy script.

## Template Sync Rule

The original standalone sample template lives at:

```text
dotfiles_private\windows\bootstrap_windows\native_cpp\templates\prappy
```

It uses the former standalone layout. Do not copy its path assumptions into
`apps/prappy`; this app's scripts resolve their own app root and the monorepo's
sibling dotfiles checkout.
