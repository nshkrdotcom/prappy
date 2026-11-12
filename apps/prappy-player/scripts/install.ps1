#Requires -Version 7.0
param([ValidateSet('Debug','Release')][string]$Config='Debug',
    [string]$Destination=(Join-Path $PSScriptRoot '../build/Release'),[switch]$NoBuild)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
if(!$NoBuild){ & "$PSScriptRoot/build.ps1" -Config $Config }
$source=[IO.Path]::GetFullPath((Join-Path $app "build/$Config"))
$target=[IO.Path]::GetFullPath($Destination)
$manifest=Get-Content -LiteralPath "$source/player-build.json" -Raw | ConvertFrom-Json
if($manifest.configuration -ne $Config){throw 'Build configuration does not match install request.'}
$names=@('PrappyPlayer.exe','PrappyPlayer.pdb','PrappyPlayer.ico','PrappyReports.exe','PrappyReports.pdb','sentry-crash.exe','sentry-wer.dll')
foreach($name in $names){
    if((Get-FileHash -LiteralPath "$source/$name").Hash -ne $manifest.files.$name){throw "Package hash mismatch: $name"}
}
if((Get-AuthenticodeSignature -LiteralPath "$source/PrappyPlayer.exe").Status -ne 'Valid'){throw 'Player signature invalid.'}
foreach($name in @('PrappyReports.exe','sentry-crash.exe','sentry-wer.dll')){if((Get-AuthenticodeSignature -LiteralPath "$source/$name").Status -ne 'Valid'){throw "Invalid reporting component signature: $name"}}
if($source -eq $target){Write-Host "Verified $Config package already at $target."; return}
if(Get-Process PrappyPlayer -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq "$target\PrappyPlayer.exe" -or $_.Path -eq "$target/PrappyPlayer.exe"}){
    throw 'Close the installed player before replacing its executable; user process was not signalled.'
}
$collector=Get-Process PrappyReports -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq "$target\PrappyReports.exe" -or $_.Path -eq "$target/PrappyReports.exe"}
if($collector){
    $health=Invoke-RestMethod 'http://127.0.0.1:19876/health' -TimeoutSec 2
    if([IO.Path]::GetFullPath($health.root) -ne [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Prappy/Reports'))){throw 'Unexpected collector root; refusing to stop another collector.'}
    Invoke-RestMethod -Method Post 'http://127.0.0.1:19876/shutdown' -TimeoutSec 2 | Out-Null
    foreach($process in $collector){if(!$process.WaitForExit(5000)){throw 'Local collector did not stop for installation.'}}
}
$null=New-Item -ItemType Directory -Force $target
# Archive only replaced package files, never copy or rewrite user state.
$archive=Join-Path $app ('build/install-history/'+[guid]::NewGuid().ToString('N'))
foreach($name in @($names)+@('player-build.json')){
    if(Test-Path -LiteralPath "$target/$name"){
        $null=New-Item -ItemType Directory -Force $archive
        Copy-Item -LiteralPath "$target/$name" -Destination $archive
    }
}
foreach($name in $names){Copy-Item -LiteralPath "$source/$name" -Destination $target -Force}
foreach($name in @('fonts','licenses')){Copy-Item -LiteralPath "$source/$name" -Destination $target -Recurse -Force}
Copy-Item -LiteralPath "$source/player-build.json" -Destination $target -Force
if(Test-Path -LiteralPath "$target/procdump64.exe"){
    $null=New-Item -ItemType Directory -Force $archive
    Copy-Item -LiteralPath "$target/procdump64.exe" -Destination $archive
    Remove-Item -LiteralPath "$target/procdump64.exe"
}
foreach($name in $names){
    if((Get-FileHash -LiteralPath "$target/$name").Hash -ne $manifest.files.$name){throw "Installed hash mismatch: $name"}
}
Write-Host "Installed $Config package with matching symbols into $target; state files unchanged."
