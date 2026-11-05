#Requires -Version 7.0
param([string]$BuildVariant='Release')
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$state=Join-Path $app ('build/relaunch-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$state/gate" | Out-Null
'debug_log=true' | Set-Content "$state/player.ini" -Encoding utf8NoBOM
('x'*32)|Set-Content "$state/token"
@{url='http://127.0.0.1:1';token_file="$state/token";library=''}|ConvertTo-Json|Set-Content "$state/sampling-connection.json"
Set-Content "$state/gate/hold" 'hold'
$env:PRAPPY_TEST_INSTANCE='Local\PrappyPlayer.Relaunch.'+[Guid]::NewGuid().ToString('N')
$env:PRAPPY_TEST_EXIT_GATE="$state/gate"
$exe=Join-Path $app "build/$BuildVariant/PrappyPlayer.exe"
$first=$null; $second=$null
function Wait-Window($process) {
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt 8) {
        $process.Refresh()
        if($process.HasExited) { throw 'Relaunch exited instead of opening a window.' }
        if($process.MainWindowHandle -ne [IntPtr]::Zero) { return }
        Start-Sleep -Milliseconds 25
    }
    throw 'Relaunch did not open a window.'
}
try {
    $first=Start-Process $exe -WindowStyle Hidden -PassThru -ArgumentList '--window-test',('"{0}"' -f $state)
    Wait-Window $first
    $null=$first.CloseMainWindow()
    $ready=Join-Path "$state/gate" ('cleanup-'+$first.Id+'.ready')
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while (!(Test-Path -LiteralPath $ready)) {
        if($timer.Elapsed.TotalSeconds -gt 8) { throw 'First instance did not reach window cleanup.' }
        Start-Sleep -Milliseconds 25
    }
    $first.Refresh()
    if($first.HasExited) { throw 'Regression did not hold the old instance in cleanup.' }
    $second=Start-Process $exe -WindowStyle Hidden -PassThru -ArgumentList '--window-test',('"{0}"' -f $state)
    Wait-Window $second
    Write-Host 'PASS new instance opens while the previous instance is held in window cleanup.'
    Remove-Item -LiteralPath "$state/gate/hold"
    if(!$first.WaitForExit(5000) -or $first.ExitCode -ne 0) { throw 'Old instance did not finish cleanup.' }
    if($env:PRAPPY_LEAK_REPORT -and (Get-Content -LiteralPath $env:PRAPPY_LEAK_REPORT -Raw) -notmatch '^PASS no CRT leaks\s*$') { throw 'Old instance reported CRT leaks.' }
    $null=$second.CloseMainWindow()
    if(!$second.WaitForExit(5000) -or $second.ExitCode -ne 0) { throw 'Relaunched player did not close cleanly.' }
    if($env:PRAPPY_LEAK_REPORT -and (Get-Content -LiteralPath $env:PRAPPY_LEAK_REPORT -Raw) -notmatch '^PASS no CRT leaks\s*$') { throw 'Relaunched instance reported CRT leaks.' }
    Write-Host 'PASS both instances close cleanly without sharing the production instance lock.'
} finally {
    Remove-Item Env:PRAPPY_TEST_INSTANCE,Env:PRAPPY_TEST_EXIT_GATE -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath "$state/gate/hold" -ErrorAction SilentlyContinue
    foreach($process in @($first,$second)) { if($process -and !$process.HasExited) { $null=$process.CloseMainWindow(); if(!$process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force } } }
}
