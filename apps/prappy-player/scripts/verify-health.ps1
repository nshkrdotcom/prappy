#Requires -Version 7.0
param([Parameter(Mandatory)][string]$Manifest,[Parameter(Mandatory)][string]$StateSource,[switch]$SkipBuild,[switch]$VerifyCommands, [ValidateRange(1,10)][int]$Repetitions=2,
    [ValidatePattern('^[A-Za-z0-9-]*$')][string]$VariantSuffix='',
    [ValidateSet('Release','Debug','Debug-Asan')][string[]]$Variants=@('Release','Debug','Debug-Asan'))
$ErrorActionPreference='Stop'
$app = Split-Path $PSScriptRoot -Parent
$releaseSuffix=if($VariantSuffix){'Memory-'+$VariantSuffix}else{'Memory'}
if (!$SkipBuild) {
    & "$PSScriptRoot/build.ps1" -Config Release -VariantSuffix $releaseSuffix
    & "$PSScriptRoot/build.ps1" -Config Debug -Analyze -VariantSuffix $VariantSuffix
    & "$PSScriptRoot/build.ps1" -Config Debug -VariantSuffix $VariantSuffix
    & "$PSScriptRoot/build.ps1" -Config Debug -Asan -VariantSuffix $VariantSuffix
}
$out = Join-Path $app 'build/health'
New-Item -ItemType Directory -Force $out | Out-Null
$previousReport = $env:PRAPPY_LEAK_REPORT
# UI helpers use script-scoped process/input state. Give each verifier its own
# PowerShell script scope, including its native type definitions and callbacks.
function Invoke-Verifier([string]$Name, [object[]]$Arguments) {
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot $Name) @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Name failed (exit $LASTEXITCODE)." }
}
function Require-LeakReport([string]$Path) {
    if (!(Test-Path -LiteralPath $Path) -or (Get-Content -LiteralPath $Path -Raw) -notmatch '^PASS no CRT leaks\s*$') {
        throw "Missing or failed CRT leak report: $Path"
    }
    Write-Host "PASS CRT leak report: $Path"
}
try {
    # Static analysis is a compile-time check; exercise Release behavior once.
    foreach ($variant in $Variants) {
        $buildVariant = $variant
        if ($variant -eq 'Release') { $buildVariant += "-$releaseSuffix" }
        elseif ($VariantSuffix) { $buildVariant += "-$VariantSuffix" }
        for ($cycle=1; $cycle -le $Repetitions; $cycle++) {
            $report = Join-Path $out "$variant-playback-$cycle-$([Guid]::NewGuid().ToString('N')).log"
            $env:PRAPPY_LEAK_REPORT = $report
            Invoke-Verifier 'test-state.ps1' @('-BuildVariant', $buildVariant)
            if ($variant -eq 'Debug') { Require-LeakReport $report }
        }
        $report = Join-Path $out "$variant-sampling-$([Guid]::NewGuid().ToString('N')).log"
        $env:PRAPPY_LEAK_REPORT = "$report.crt"
        $process = Start-Process -FilePath "$app/build/$buildVariant/tests/PrappyStateTests.exe" -WindowStyle Hidden -PassThru `
            -RedirectStandardError "$report.stderr" -ArgumentList '--sampling-test',('"{0}"' -f $Manifest),('"{0}"' -f $report)
        if (!$process.WaitForExit(120000)) { Stop-Process -Id $process.Id -Force; throw 'Native sampling regression timed out.' }
        if ($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $report) -or (Get-Content -LiteralPath $report -Raw) -notmatch 'ALL PASS') { throw "Native sampling regression failed: $report" }
        if ($variant -eq 'Debug') { Require-LeakReport "$report.crt" }
        $report = Join-Path $out "$variant-ui-$([Guid]::NewGuid().ToString('N')).log"
        $env:PRAPPY_LEAK_REPORT = $report
        $editorArguments = @('-Manifest', $Manifest, '-BuildVariant', $buildVariant, '-VerifySaveExport')
        if ($VerifyCommands) { $editorArguments += '-VerifyCommands' }
        Invoke-Verifier 'verify-native-editor.ps1' $editorArguments
        if ($variant -eq 'Debug') { Require-LeakReport $report }
        $report = Join-Path $out "$variant-features-$([Guid]::NewGuid().ToString('N')).log"
        $env:PRAPPY_LEAK_REPORT = $report
        Invoke-Verifier 'verify-features.ps1' @('-StateSource', $StateSource, '-BuildVariant', $buildVariant)
        if ($variant -eq 'Debug') { Require-LeakReport $report }
        $report = Join-Path $out "$variant-window-$([Guid]::NewGuid().ToString('N')).log"
        $env:PRAPPY_LEAK_REPORT = $report
        Invoke-Verifier 'verify-window.ps1' @('-BuildVariant', $buildVariant)
        if ($variant -eq 'Debug') { Require-LeakReport $report }
        $report = Join-Path $out "$variant-relaunch-$([Guid]::NewGuid().ToString('N')).log"
        # Only Debug builds emit CRT reports; the relaunch check validates both exits.
        $env:PRAPPY_LEAK_REPORT = if ($variant -eq 'Debug') { $report } else { $null }
        Invoke-Verifier 'verify-relaunch.ps1' @('-BuildVariant', $buildVariant)
        if ($variant -eq 'Debug') { Require-LeakReport $report }
    }
} finally { $env:PRAPPY_LEAK_REPORT = $previousReport }
Write-Host 'PASS strict builds, static analysis, playback stress, catalog/history/repeat state and UI checks, Debug CRT leaks and ASan.'
