#Requires -Version 7.0
param([string]$Root=(Join-Path $env:LOCALAPPDATA 'Prappy/Reports'))
$ErrorActionPreference='Stop'
$symbols=[IO.Path]::GetFullPath((Join-Path $Root 'symbols'))
if(!(Test-Path -LiteralPath $symbols)){return}
$referenced=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$incidents=Join-Path $Root 'incidents'
if(Test-Path -LiteralPath $incidents){
    foreach($file in Get-ChildItem -LiteralPath $incidents -Recurse -File | Where-Object {$_.Name -in 'event.json','fatal.json'}){
        # Do not prune if evidence is unreadable or incomplete.
        try{$report=Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json -AsHashtable}catch{Write-Warning 'Unreadable incident metadata; retaining all symbols.';return}
        if($report.ContainsKey('build_id')){$null=$referenced.Add([string]$report['build_id'])}
        if($report.ContainsKey('tags') -and $report['tags'].ContainsKey('build_id')){$null=$referenced.Add([string]$report['tags']['build_id'])}
    }
}
$archives=@(Get-ChildItem -LiteralPath $symbols -Directory | Where-Object {$_.Name -match '^[0-9a-f]{64}$' -and !($_.Attributes -band [IO.FileAttributes]::ReparsePoint)} | Sort-Object LastWriteTime -Descending)
for($i=10;$i -lt $archives.Count;$i++){
    if($referenced.Contains($archives[$i].Name)){continue}
    $target=[IO.Path]::GetFullPath($archives[$i].FullName)
    if(![IO.Path]::GetDirectoryName($target).Equals($symbols,[StringComparison]::OrdinalIgnoreCase)){throw 'Symbol cleanup escaped its archive root.'}
    Remove-Item -LiteralPath $target -Recurse -Force
}
