#Requires -Version 7.0
param([ValidateSet('Release','Debug')][string]$Config='Release', [switch]$Run)
$ErrorActionPreference='Stop'
$app = Split-Path $PSScriptRoot -Parent
$candidates = @(
    "$env:DOTFILES_PRIVATE\windows\bootstrap_windows\native_cpp",
    "$PSScriptRoot\..\..\..\..\dotfiles_private\windows\bootstrap_windows\native_cpp",
    "$env:USERPROFILE\projects\dotfiles_private\windows\bootstrap_windows\native_cpp"
)
$common = $candidates | ForEach-Object { Join-Path $_ 'NativeCpp.Common.ps1' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$common) { throw 'Set DOTFILES_PRIVATE to the dotfiles_private checkout containing the native C++ tooling.' }
. $common
Assert-NativeCppToolchain
$build = Join-Path $app "build\$Config"
& cmake -S $app -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed. Stop and report this error.' }
& cmake --build $build --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed. Stop and report this error.' }
Write-Host "Built $build\PrappyPlayer.exe"
if ($Run) { Start-Process -FilePath "$build\PrappyPlayer.exe" -WorkingDirectory $build -WindowStyle Hidden }
