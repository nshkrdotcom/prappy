#Requires -Version 7.0
param([string]$BuildVariant='Release')
$ErrorActionPreference='Stop'
$app = Split-Path $PSScriptRoot -Parent
$fixture = Join-Path $app 'build/dedup-test/scraper/library'
if (!(Test-Path -LiteralPath "$fixture/player.ini")) { throw 'Run test-dedup.ps1 first to generate audio fixtures.' }
$stateOutput = Join-Path $app 'build/state-tests'
New-Item -ItemType Directory -Force $stateOutput | Out-Null
$output = Join-Path $stateOutput "state-$BuildVariant.log"
$process = Start-Process -FilePath "$app/build/$BuildVariant/PrappyPlayer.exe" -WindowStyle Hidden -PassThru `
    -RedirectStandardError "$stateOutput/state-stderr-$BuildVariant.log" `
    -ArgumentList '--state-test',('"{0}"' -f "$fixture/player.ini"),('"{0}"' -f $output)
if (!$process.WaitForExit(120000)) { Stop-Process -Id $process.Id -Force; throw 'State regression timed out.' }
$result = Get-Content -LiteralPath $output -Raw
Write-Host $result
if ($process.ExitCode -ne 0 -or $result -notmatch 'ALL PASS') { throw 'History/catalog/session/repeat regression failed.' }
