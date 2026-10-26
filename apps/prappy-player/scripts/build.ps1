#Requires -Version 7.0
param([ValidateSet('Release','Debug')][string]$Config='Release', [switch]$Run,
    [switch]$Analyze, [switch]$Asan)
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
$variant = $Config
if ($Analyze) { $variant += '-Analyze' }
if ($Asan) { $variant += '-Asan' }
$build = Join-Path $app "build\$variant"
& cmake -S $app -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DPRAPPY_ANALYZE:BOOL=$($Analyze.IsPresent)" "-DPRAPPY_ASAN:BOOL=$($Asan.IsPresent)"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed. Stop and report this error.' }
& cmake --build $build --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed. Stop and report this error.' }
if ($Asan) {
    $compiler = (Get-Command cl.exe).Source
    $runtime = Join-Path (Split-Path $compiler) 'clang_rt.asan_dynamic-x86_64.dll'
    if (!(Test-Path -LiteralPath $runtime)) { throw "ASan runtime missing: $runtime. Install the Visual Studio C++ AddressSanitizer component." }
    Copy-Item -LiteralPath $runtime -Destination $build -Force
}
Write-Host "Built $build\PrappyPlayer.exe"
if ($Run) { Start-Process -FilePath "$build\PrappyPlayer.exe" -WorkingDirectory $build -WindowStyle Hidden }
