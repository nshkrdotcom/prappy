# Reproducible Windows Tooling

Prappy is the sample app. The reusable setup lives in:

```text
C:\Users\windo\projects\dotfiles_private\windows\bootstrap_windows\native_cpp
```

The scripts in this repo call into that shared tooling through
`scripts\_tooling.ps1`.

## Why The Wrapper Scripts Are Small

The app repo should stay focused on app code. The Windows environment setup,
toolchain discovery, dependency handling, and template generation belong in the
dotfiles repo so they can be reused on a clean Windows 11 machine.

## Regenerate The App From Dotfiles

From the dotfiles repo:

```powershell
cd C:\Users\windo\projects\dotfiles_private
pwsh windows\bootstrap_windows\Initialize-PrappyNativeSample.ps1 -Force
```

Use `-Force` only when you intentionally want template files copied into this
repo.

## Full Dotfiles Setup Path

```powershell
cd C:\Users\windo\projects\dotfiles_private
pwsh windows\bootstrap_windows\Invoke-PrappyNativeSample.ps1 All -Force -SmokeTest
```

For screenshot verification through the wrapper:

```powershell
pwsh windows\bootstrap_windows\Invoke-PrappyNativeSample.ps1 Run -Visualization Oahu -SmokeTest -ScreenshotSmoke
```

## If The Tooling Cannot Be Found

Set this in the current PowerShell session:

```powershell
$env:DOTFILES_PRIVATE = "C:\Users\windo\projects\dotfiles_private"
```

Then rerun the failed Prappy script.

## Template Sync Rule

When app structure or script behavior changes, mirror the relevant source into:

```text
dotfiles_private\windows\bootstrap_windows\native_cpp\templates\prappy
```

That is what keeps the next clean-machine setup reproducible.
