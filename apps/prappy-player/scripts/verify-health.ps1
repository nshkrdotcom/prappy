#Requires -Version 7.0
param([switch]$SkipBuild, [ValidateRange(1,10)][int]$Repetitions=2,
    [ValidatePattern('^[A-Za-z0-9-]*$')][string]$VariantSuffix='')
$ErrorActionPreference='Stop'
$app = Split-Path $PSScriptRoot -Parent
if (!$SkipBuild) {
    & "$PSScriptRoot/build.ps1" -VariantSuffix $VariantSuffix
    & "$PSScriptRoot/build.ps1" -Analyze -VariantSuffix $VariantSuffix
    & "$PSScriptRoot/build.ps1" -Config Debug -VariantSuffix $VariantSuffix
    & "$PSScriptRoot/build.ps1" -Config Debug -Asan -VariantSuffix $VariantSuffix
}
$out = Join-Path $app 'build/health'
New-Item -ItemType Directory -Force $out | Out-Null
$previousReport = $env:PRAPPY_LEAK_REPORT
function Require-LeakReport([string]$Path) {
    if (!(Test-Path -LiteralPath $Path) -or (Get-Content -LiteralPath $Path -Raw) -notmatch '^PASS no CRT leaks\s*$') {
        throw "Missing or failed CRT leak report: $Path"
    }
    Write-Host "PASS CRT leak report: $Path"
}
try {
    # Static analysis is a compile-time check; exercise Release behavior once.
    foreach ($variant in @('Release','Debug','Debug-Asan')) {
        $buildVariant = $variant
        if ($VariantSuffix) { $buildVariant += "-$VariantSuffix" }
        for ($cycle=1; $cycle -le $Repetitions; $cycle++) {
            $report = Join-Path $out "$variant-playback-$cycle-$([Guid]::NewGuid().ToString('N')).log"
            $env:PRAPPY_LEAK_REPORT = $report
            & "$PSScriptRoot/test-dedup.ps1" -BuildVariant $buildVariant
            if ($variant -eq 'Debug') { Require-LeakReport $report }
        }
        $report = Join-Path $out "$variant-state-$([Guid]::NewGuid().ToString('N')).log"
        $env:PRAPPY_LEAK_REPORT = $report
        & "$PSScriptRoot/test-state.ps1" -BuildVariant $buildVariant
        if ($variant -eq 'Debug') { Require-LeakReport $report }
        $report = Join-Path $out "$variant-ui-$([Guid]::NewGuid().ToString('N')).log"
        $env:PRAPPY_LEAK_REPORT = $report
        & "$PSScriptRoot/verify-features.ps1" -BuildVariant $buildVariant
        if ($variant -eq 'Debug') { Require-LeakReport $report }
        $report = Join-Path $out "$variant-window-$([Guid]::NewGuid().ToString('N')).log"
        $env:PRAPPY_LEAK_REPORT = $report
        & "$PSScriptRoot/verify-window.ps1" -BuildVariant $buildVariant
        if ($variant -eq 'Debug') { Require-LeakReport $report }
    }
} finally { $env:PRAPPY_LEAK_REPORT = $previousReport }
Write-Host 'PASS strict builds, static analysis, playback stress, catalog/history/repeat state and UI checks, Debug CRT leaks and ASan.'
