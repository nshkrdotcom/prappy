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
$collector=Get-Process PrappyReports -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq [IO.Path]::GetFullPath((Join-Path $build 'PrappyReports.exe'))}
if($collector){
    if(Get-Process PrappyPlayer -ErrorAction SilentlyContinue){throw 'Close the player before rebuilding its active reporting helper.'}
    foreach($process in $collector){
        $listeners=Get-NetTCPConnection -OwningProcess $process.Id -State Listen -ErrorAction SilentlyContinue | Where-Object {$_.LocalAddress -eq '127.0.0.1' -and $_.LocalPort -in 19876,19877}
        foreach($listener in $listeners){Invoke-RestMethod -Method Post "http://127.0.0.1:$($listener.LocalPort)/shutdown" -TimeoutSec 2 | Out-Null}
        if(!$process.WaitForExit(5000)){throw 'Development report collector did not stop before rebuild.'}
    }
}
& "$PSScriptRoot/reporting-deps.ps1"
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
$packageFiles=@('PrappyPlayer.exe','PrappyPlayer.pdb','PrappyPlayer.ico','PrappyReports.exe','PrappyReports.pdb','sentry-crash.exe','sentry-wer.dll')
$hashes=[ordered]@{}
foreach($name in $packageFiles) {
    $hashes[$name]=(Get-FileHash -LiteralPath (Join-Path $build $name) -Algorithm SHA256).Hash
}
$buildCommit=(& git -C $app rev-parse HEAD)
$buildDirty=[bool](& git -C $app status --porcelain)
$identity=[ordered]@{configuration=$Config;commit=$buildCommit;source_dirty=$buildDirty;files=$hashes}|ConvertTo-Json -Compress -Depth 4
$buildId=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($identity))).ToLowerInvariant()
[ordered]@{build_id=$buildId;configuration=$Config;commit=$buildCommit;source_dirty=$buildDirty;reporting=@{sdk='sentry-native 0.17.6';exception_traces='boost 1.91.0';collector='http://127.0.0.1:19876'};files=$hashes} |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $build 'player-build.json') -Encoding utf8
$symbolArchive=Join-Path $env:LOCALAPPDATA "Prappy/Reports/symbols/$buildId"
$null=New-Item -ItemType Directory -Force -Path $symbolArchive
foreach($name in @($packageFiles)+@('player-build.json')){Copy-Item -LiteralPath (Join-Path $build $name) -Destination $symbolArchive -Force}
foreach($name in @('prappy_crash_probe.exe','prappy_crash_probe.pdb')){Copy-Item -LiteralPath (Join-Path $build "tests/$name") -Destination $symbolArchive -Force}
& "$PSScriptRoot/prune-report-symbols.ps1"
Write-Host "Built $build\PrappyPlayer.exe"
if ($Run) { Start-Process -FilePath "$build\PrappyPlayer.exe" -WorkingDirectory $build -WindowStyle Hidden }
