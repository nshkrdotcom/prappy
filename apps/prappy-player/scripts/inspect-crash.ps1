#Requires -Version 7.0
param([string]$Incident,[string]$Root=(Join-Path $env:LOCALAPPDATA 'Prappy/Reports'),[string]$BuildVariant='Release')
$ErrorActionPreference='Stop'
if(!$Incident){
    $latest=Get-ChildItem -LiteralPath (Join-Path $Root 'incidents') -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if(!$latest){throw 'No collected incidents found.'}
    $Incident=$latest.FullName
}elseif(!(Test-Path -LiteralPath $Incident)){$Incident=Join-Path (Join-Path $Root 'incidents') $Incident}
$tool=Join-Path $PSScriptRoot "../build/$BuildVariant/PrappyReports.exe"
& $tool --analyze ([IO.Path]::GetFullPath($Incident)) ([IO.Path]::GetFullPath($Root))
if($LASTEXITCODE -ne 0){throw 'Incident analysis failed; check its report files and symbol archive.'}
