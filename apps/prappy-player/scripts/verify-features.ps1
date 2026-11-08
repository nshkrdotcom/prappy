#Requires -Version 7.0
param([Parameter(Mandatory)][string]$StateSource,[string]$BuildVariant='Release-Indexed')
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$source=(Resolve-Path -LiteralPath $StateSource).Path
$connection=Get-Content (Join-Path $source 'sampling-connection.json') -Raw|ConvertFrom-Json
$uri=[uri]$connection.url
if($uri.Host-ne '127.0.0.1' -or $uri.Port-eq 8765){throw 'Use a closed isolated native test state, never the production backend.'}
$headers=@{Authorization='Bearer '+[IO.File]::ReadAllText($connection.token_file).Trim()}
$caps=Invoke-RestMethod -Uri ($connection.url+'/v1/capabilities') -Headers $headers
if($caps.protocol-ne 3){throw 'Feature checks require the native protocol.'}
$catalog=Invoke-RestMethod -Uri ($connection.url+'/v1/catalog') -Headers $headers
if($catalog.items.Count-lt 2){throw 'Use a catalog with at least two sources.'}
$stateDirectory=Join-Path $app ('build/feature-ui/native-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory $stateDirectory
$before=@{}
foreach($name in @('player.ini','sampling-connection.json')){
 $path=Join-Path $source $name;$before[$name]=(Get-FileHash -LiteralPath $path).Hash
 Copy-Item -LiteralPath $path -Destination $stateDirectory
 if((Get-FileHash -LiteralPath $path).Hash-ne $before[$name] -or (Get-FileHash (Join-Path $stateDirectory $name)).Hash-ne $before[$name]){throw 'Source state changed during capture.'}
}
if(Test-Path (Join-Path $source 'evaluation-state.json')){
 $marker=Get-Content (Join-Path $source 'evaluation-state.json') -Raw|ConvertFrom-Json
 $marker.state_root=$stateDirectory
 [IO.File]::WriteAllText("$stateDirectory/evaluation-state.json",($marker|ConvertTo-Json -Depth 20),[Text.UTF8Encoding]::new($false))
}
$history=@('PRAPPY_HISTORY 1','NEXT_ID 3','REPEAT 0')
for($index=0;$index-lt 2;$index++){
 $item=$catalog.items[$index]
 $path=Join-Path $stateDirectory ('sources/'+$item.library_id+'/'+$item.capture_id+'.source')
 $history+=('ENTRY '+($index+1)+' '+(1700000000+$index)+' 0 '+($path|ConvertTo-Json -Compress))
}
$history+=@('TRAIL 1 2','CURSOR 2')
[IO.File]::WriteAllLines("$stateDirectory/player-history.txt",$history,[Text.UTF8Encoding]::new($false))
# Fresh preferences and two native visits make UI checks independent of user drafts/history.
. "$PSScriptRoot/ui-test-helpers.ps1"
$script:inputSequence=0;$hwnd=[IntPtr]::Zero;$process=$null
function Start-Player {
 foreach($name in @('ui-status.txt','ui-command.txt')){Remove-Item -LiteralPath (Join-Path $stateDirectory $name) -ErrorAction SilentlyContinue}
 $script:inputSequence=0;$script:hwnd=[IntPtr]::Zero
 $script:process=Start-Process "$app/build/$BuildVariant/PrappyPlayer.exe" -WindowStyle Hidden -PassThru -ArgumentList '--window-test',('"'+$stateDirectory+'"')
 $null=Wait-Ui {param($s)$s.Catalog.sources-eq $catalog.items.Count -and !$s.Pending -and $s.Duration-gt 0} 'native indexed session ready' 40
 $process.Refresh();$script:hwnd=$process.MainWindowHandle
}
function Stop-Player {
 if($process -and !$process.HasExited){
  $process.Refresh();$null=[FeatureNative]::PostMessage($process.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
  if(!$process.WaitForExit(5000)){Stop-Process -Id $process.Id;throw 'Owned feature player did not close.'}
 }
 if($process){Require ($process.ExitCode-eq 0) 'clean native feature shutdown';$script:process=$null}
}
try {
 Start-Player
 if(!(Read-Ui).Paused){Click play;$null=Wait-Ui {param($s)$s.Paused} 'paused native startup'}
 Capture startup;Check-HeaderText startup (Read-Ui)
 foreach($name in @('back','next','repeat','refresh')){
  $layout=Read-Ui;Require ($layout.Controls[$name].Top-eq $layout.Controls['play'].Top -and $layout.Controls[$name].Bottom-eq $layout.Controls['play'].Bottom) "aligned $name transport"
 }
 $layout=Read-Ui
 Require ($layout.Controls['volume-value'].Right-lt $layout.Controls['volume'].Left) 'volume value cannot be covered by its slider'
 $initial=Read-Ui
 foreach($mode in @(0,1,2)){
  Click repeat;$next=Wait-Ui {param($s)$s.Repeat-eq (($initial.Repeat+$mode+1)%3)} 'repeat mode persists'
 }
 if($initial.Cursor-gt 0){
  $count=$initial.Count;Click back
  $null=Wait-Ui {param($s)!$s.Pending} 'Back native load'
  Click next;$null=Wait-Ui {param($s)!$s.Pending} 'Forward native load'
  Require ((Read-Ui).Count-eq $count) 'Back and Forward retain visit identities'
  if(!(Read-Ui).Paused){Click play}
 }
 Click list;$null=Wait-Ui {param($s)$s.Height-ge 452 -and $s.Controls.ContainsKey('order')} 'expanded browser'
 if((Read-Ui).History){Click Library}
 Click filters;Click filter-reset;Dismiss-Popup
 $all=Wait-Ui {param($s)$s.Visible-eq $catalog.items.Count} 'whole native catalog visible'
 foreach($mode in @(0,1,2,3)){
  Select-Combo order $mode
  $rows=Wait-Ui {param($s)$s.SortOrder-eq $mode} "sort $mode"
  if($all.Visible-gt 100){Require ($rows.DrawnTracks-le [Math]::Ceiling($rows.Height/18)+4) 'only viewport tracks are drawn'}
 }
 Select-Combo order 1
 if((Read-Ui).Controls.ContainsKey('group-0')){
  $expanded=Read-Ui;Click group-0;Send-UiInput 'MOVE 0 0'
  $collapsed=Read-Ui;Require ($collapsed.ScrollMaxY-le $expanded.ScrollMaxY) 'native library group collapses'
  Click group-0
 }
 Search ('prappy-no-result-'+[guid]::NewGuid().ToString('N'))
 $null=Wait-Ui {param($s)$s.Visible-eq 0 -and $s.DrawnTracks-eq 0} 'no stale rows in empty search'
 $unicode=[string][char]0x65e5+[char]0x672c;Search ($unicode+' "quoted" \ audio')
 $null=Wait-Ui {param($s)$s.Session.Contains('quoted') -and $s.Session.Contains('audio')} 'quoted Unicode search is preserved'
 Click Clear;$null=Wait-Ui {param($s)$s.Visible-eq $all.Visible} 'search clears without losing sources'
 Click search;Send-UiInput ('NATIVE_EDIT "'+$unicode.Substring(0,1)+'"')
 $null=Wait-Ui {param($s)$s.Composing} 'SDL composition reaches search'
 foreach($key in @('ENTER','ESC','SPACE')){Send-UiInput "NATIVE_KEY $key 1";Send-UiInput "NATIVE_KEY $key 0"}
 Require ((Read-Ui).Paused) 'composition keys preserve paused playback'
 Send-UiInput ('NATIVE_TEXT "'+$unicode+'"');Send-UiInput 'NATIVE_EDIT ""'
 $null=Wait-Ui {param($s)!$s.Composing -and $s.Session.Contains($unicode)} 'native Unicode commit reaches search'
 Click Clear
 $term=[regex]::Match($catalog.items[0].title,'[\p{L}]{5,}').Value
 if(!$term){throw 'Choose a feature catalog whose first title contains a searchable word.'}
 Search $term
 $matched=Wait-Ui {param($s)$s.Visible-gt 0 -and $s.Controls.Keys.Where({$_-like 'library-*'}).Count-gt 0} 'native title search has matches'
 $row=$matched.Controls.GetEnumerator()|Where-Object {$_.Key-like 'library-*' -and $_.Value.Top-ge 126 -and $_.Value.Bottom-lt $matched.Controls['search'].Top}|Sort-Object {$_.Value.Top}|Select-Object -First 1
 Require ($null-ne $row) 'matched native source is visible'
 Click $row.Key
 $null=Wait-Ui {param($s)!$s.Pending -and $s.Count-eq $matched.Count+1 -and $s.Session.Contains($term)} 'native search selection retains query and adds one visit'
 if(!(Read-Ui).Paused){Click play}
 Capture matched
 Click Clear
 Click filters;Select-Combo availability 2;Dismiss-Popup
 $null=Wait-Ui {param($s)$s.Visible-eq 0} 'offline filter excludes online sources'
 Click filters;Click filter-reset;Dismiss-Popup
 $null=Wait-Ui {param($s)$s.Visible-eq $all.Visible} 'filter reset restores native sources'
 foreach($view in @(1,2,3,0)){
  Select-Combo view $view;$null=Wait-Ui {param($s)$s.View-eq $view} "retained-history view $view"
 }
 Check-DirectoryRow narrow
 Click directory-chip-0
 $included=Wait-Ui {param($s)$s.Included-eq 1 -and $s.Visible-gt 0} 'native library include'
  Click directories
  $null=Wait-Ui {param($s)$s.Controls.ContainsKey('directory-pin-0')} 'directory picker'
  Click directory-pin-0
  $null=Wait-Ui {param($s)$s.Pins-eq 1} 'library pin'
  Click directory-all;Dismiss-Popup
 $null=Wait-Ui {param($s)$s.Visible-eq $all.Visible -and $s.Included-eq 0} 'clear include retains native catalog'
 $row=Read-Ui;Context audio-info
 Require ((Read-Ui).Controls.ContainsKey('copy-path') -and (Read-Ui).Controls.ContainsKey('copy-wsl')) 'native original path actions'
 Click queue-next;$queued=Wait-Ui {param($s)$s.Queued-eq $row.Queued+1 -and $s.Count-eq $row.Count} 'queue does not add visits'
 Click next;$null=Wait-Ui {param($s)!$s.Pending -and $s.Queued-eq $queued.Queued-1 -and $s.Count-eq $queued.Count+1} 'Next consumes queued native source'
 if(!(Read-Ui).Paused){Click play}
 $ready=Wait-Ui {param($s)$s.Waveform-gt 0 -and $s.Paused} 'bounded native overview'
 $seek=$ready.Controls['seek'];Click-At seek ($seek.Left+($seek.Right-$seek.Left)*.7) $seek.Y
 $null=Wait-Ui {param($s)$s.Paused -and [Math]::Abs($s.MarkerRatio-.7)-lt .01} 'native waveform seek retains pause'
 Send-UiInput DOWN;$held=Read-Ui;Start-Sleep -Milliseconds 150;$stationary=Read-Ui;Send-UiInput UP
 Require ($stationary.SeekSequence-eq $held.SeekSequence) 'stationary seek does not repeat commands'
 Require ($stationary.WaveVertices-le $stationary.Width*64+512) 'waveform drawing remains viewport bounded'
 $divider=Move-Control transport-resize;$preferred=(Read-Ui).PanelPreferred
 Send-UiInput DOWN;Send-UiInput ('MOVE '+$divider.X+' '+($divider.Y-110));Send-UiInput UP
 $null=Wait-Ui {param($s)$s.PanelPreferred-gt $preferred} 'waveform resize'
 $position=(Read-Ui).Position
 $null=Move-Control transport-resize;Send-UiInput DOUBLE_CLICK
 $null=Wait-Ui {param($s)$s.PanelPreferred-eq 202} 'waveform double-click reset'
 Require ([Math]::Abs((Read-Ui).Position-$position)-lt .02) 'resize preserves native position'
 $beforeReset=Read-Ui;Send-UiInput RENDER_RESET
 $null=Wait-Ui {param($s)$s.TextRasterized-gt $beforeReset.TextRasterized -and $s.WaveBuilds-gt $beforeReset.WaveBuilds} 'renderer reset restores native waveform/text'
 Click more;Click topmost
 $null=Wait-Ui {param($s)$s.Topmost} 'topmost enabled'
 Click normalize;$null=Wait-Ui {param($s)$s.Normalize} 'normalization preference enabled'
 Click strip;$saved=Wait-Ui {param($s)$s.Strip -and $s.Height-eq 86 -and $s.Waveform-gt 0} 'strip retains native transport'
 Stop-Player;Start-Player
 $restored=Wait-Ui {param($s)$s.Strip -and $s.Topmost -and $s.Normalize -and $s.Paused -and $s.Pins-eq 1} 'native preferences and paused session restore'
 Require ([Math]::Abs($restored.Position-$saved.Position)-le .02 -and $restored.Count-eq $saved.Count) 'restart preserves native visit and position'
 Send-UiInput 'KEY CTRL 1';Send-UiInput 'KEY SHIFT 1';Send-UiInput 'KEY M 1';Send-UiInput 'KEY M 0';Send-UiInput 'KEY SHIFT 0';Send-UiInput 'KEY CTRL 0'
 $null=Wait-Ui {param($s)!$s.Strip} 'strip keyboard shortcut'
 Send-UiInput 'KEY CTRL 1';Send-UiInput 'KEY SHIFT 1';Send-UiInput 'KEY T 1';Send-UiInput 'KEY T 0';Send-UiInput 'KEY SHIFT 0';Send-UiInput 'KEY CTRL 0'
 $null=Wait-Ui {param($s)!$s.Topmost} 'topmost keyboard shortcut'
 Capture restored
 Stop-Player
 foreach($name in $before.Keys){Require ((Get-FileHash -LiteralPath (Join-Path $source $name)).Hash-eq $before[$name]) "source $name untouched"}
 [pscustomobject]@{result='PASS';sources=$catalog.items.Count;state=$stateDirectory;protocol=$caps.protocol}|ConvertTo-Json
}finally{Stop-Player}
