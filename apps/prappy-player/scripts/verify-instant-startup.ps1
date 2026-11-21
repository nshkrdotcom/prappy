param([Parameter(Mandatory)][string]$Manifest,[string]$BuildVariant='Debug-Startup')
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$fixture=Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
if($fixture.url -notmatch '^http://127\.0\.0\.1:(?!8765$)[0-9]+$' -or !$fixture.gate_url){
    throw 'A disposable startup fixture is required.'
}
$stateDirectory=Join-Path $app ('build/startup-probe-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $stateDirectory
Set-Content -LiteralPath "$stateDirectory/player.ini" 'debug_log=true'
@{url=$fixture.url;token_file=$fixture.token_file;library=$fixture.library_id} |
    ConvertTo-Json | Set-Content -LiteralPath "$stateDirectory/sampling-connection.json"
$headers=@{Authorization='Bearer '+[IO.File]::ReadAllText($fixture.token_file).Trim()}
$script:inputSequence=0
$process=$null
$hwnd=[IntPtr]::Zero
. "$PSScriptRoot/ui-test-helpers.ps1"
function Start-Player {
    foreach($name in 'ui-status.txt','ui-command.txt'){
        Remove-Item -LiteralPath (Join-Path $stateDirectory $name) -ErrorAction SilentlyContinue
    }
    $script:process=Start-Process "$app/build/$BuildVariant/PrappyPlayer.exe" -WindowStyle Hidden -PassThru `
        -ArgumentList '--window-test',('"{0}"' -f $stateDirectory)
    $script:hwnd=[IntPtr]::Zero
}
function Stop-Player {
    if($process -and !$process.HasExited){
        $null=$process.CloseMainWindow()
        if(!$process.WaitForExit(5000)){Stop-Process -Id $process.Id;throw 'Fixture player did not close cleanly.'}
    }
}
try {
    Start-Player
    $null=Wait-Ui { param($ui) $ui.Catalog.sources -eq $fixture.expected_sources } 'live browse snapshot'
    Click list
    Click library-0
    $first=Wait-Ui { param($ui) $ui.Duration -gt 0 -and !$ui.Pending -and $ui.Cursor -gt 0 } 'first playback'
    if(!$first.Paused){Click play}
    $before=Wait-Ui { param($ui) $ui.Paused -and (Get-ChildItem "$stateDirectory/song-pcm-v1" -Filter '*.pcm' -ErrorAction SilentlyContinue) } 'saved local PCM'
    Stop-Player
    $null=Invoke-RestMethod -Uri "$($fixture.gate_url)/hold" -Method Post -Headers $headers
    Start-Player
    $reopened=Wait-Ui { param($ui)
        $ui.Catalog.sources -eq $fixture.expected_sources -and !$ui.Catalog.current -and
        $ui.Duration -gt 0 -and !$ui.Pending -and $ui.Paused -and !$ui.PlaybackError
    } 'catalog and exact track recovered while every backend request is held'
    Require ($reopened.Count -eq $before.Count) 'Restore reused the existing history visit'
    Click play
    $null=Wait-Ui { param($ui) $ui.Position -gt $reopened.Position+0.08 } 'cached audio plays while backend is held'
    $null=Invoke-RestMethod -Uri "$($fixture.gate_url)/release" -Method Post -Headers $headers
    $null=Wait-Ui { param($ui) $ui.Catalog.current } 'cached catalog revalidated'
    Require ((Read-Shared "$stateDirectory/player-debug.log") -match 'catalog.unchanged') 'unchanged catalog returned no replacement list'
    $changed=Invoke-RestMethod -Uri "$($fixture.gate_url)/append-source" -Method Post -Headers $headers
    $refreshed=Wait-Ui { param($ui) $ui.Catalog.current -and $ui.Catalog.sources -eq $changed.sources } 'changed catalog published'
    Require ($refreshed.Cursor -eq $before.Cursor) 'Catalog replacement preserved the current track'
    (Read-Shared "$stateDirectory/player-debug.log") -split '\r?\n' |
        Where-Object {$_ -match 'startup.first.present|catalog.local|catalog.unchanged|resume.audio.hit|play.ready'}
} finally {
    try { $null=Invoke-RestMethod -Uri "$($fixture.gate_url)/release" -Method Post -Headers $headers } catch {}
    Stop-Player
}
