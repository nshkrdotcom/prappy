#Requires -Version 7.0
param([ValidateSet('Release','Debug')][string]$Config='Debug', [switch]$Run,
    [switch]$Analyze, [switch]$Asan, [ValidatePattern('^[A-Za-z0-9-]*$')][string]$VariantSuffix='')
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
if ($VariantSuffix) { $variant += "-$VariantSuffix" }
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
    Copy-Item -LiteralPath $runtime -Destination (Join-Path $build 'tests') -Force
}
if (Test-Path -LiteralPath "$env:USERPROFILE/.config/prappy/development-signing.json") {
    & "$PSScriptRoot/sign-development.ps1" -BuildDirectory $build
}
if ($Config -eq 'Debug') {
    $tools=Join-Path $app 'build/crash-tools'
    $monitor=Join-Path $tools 'procdump64.exe'
    if (!(Test-Path -LiteralPath $monitor)) {
        New-Item -ItemType Directory -Force $tools | Out-Null
        Invoke-WebRequest 'https://download.sysinternals.com/files/Procdump.zip' -OutFile "$tools/procdump.zip"
        Expand-Archive -LiteralPath "$tools/procdump.zip" -DestinationPath $tools -Force
    }
    $signature=Get-AuthenticodeSignature -LiteralPath $monitor
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') { throw 'Invalid Microsoft ProcDump signature.' }
    Copy-Item -LiteralPath $monitor -Destination $build -Force
}
$packageFiles=@('PrappyPlayer.exe','PrappyPlayer.pdb','PrappyPlayer.ico')
if ($Config -eq 'Debug') { $packageFiles+='procdump64.exe' }
$hashes=[ordered]@{}
foreach($name in $packageFiles) {
    $hashes[$name]=(Get-FileHash -LiteralPath (Join-Path $build $name) -Algorithm SHA256).Hash
}
[ordered]@{configuration=$Config;commit=(& git -C $app rev-parse HEAD);source_dirty=[bool](& git -C $app status --porcelain);files=$hashes} |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $build 'player-build.json') -Encoding utf8
Write-Host "Built $build\PrappyPlayer.exe"
if ($Run) { Start-Process -FilePath "$build\PrappyPlayer.exe" -WorkingDirectory $build -WindowStyle Hidden }
