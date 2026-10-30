#Requires -Version 7.0
param([Parameter(Mandatory)][string]$Manifest,[string]$BuildVariant='Release',
    [switch]$StartupOnly,[switch]$WorkstationDefaults,[switch]$RecordingOnly,[switch]$MarkOnly,[switch]$WorkflowOnly,[switch]$AudioLoopback,[switch]$RegisterSource)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$fixture=Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
if($RegisterSource -and !$WorkflowOnly){throw 'RegisterSource requires WorkflowOnly and a disposable unregistered fixture.'}
$state=Join-Path $app ('build/sampling-ui-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$state/media" | Out-Null
"directory=$state/media`nextensions=wav" | Set-Content "$state/player.ini"
if(!$WorkstationDefaults){@{url=$fixture.url;token_file=$fixture.token_file;library=$fixture.library_id} | ConvertTo-Json | Set-Content "$state/sampling-connection.json"}
$initialLibrary=if($WorkstationDefaults){''}else{$fixture.library_id}
$seed=@{workspaces=@{sources=@()};active_source=$null;commands=@();open=$true;browser_library=$initialLibrary;search='';facet='';facet_value='';tab=0;offset=0;gain=.7}
if($MarkOnly -or $WorkflowOnly){
 Copy-Item -LiteralPath $fixture.mark_song -Destination "$state/media/current.wav"
 $ref=@{library_id=$fixture.library_id;capture_id=$fixture.previous_capture.id;timeline_id=$fixture.previous_capture.timeline_id}
 $draft=@{id='previous-draft';value=@{start=0;end=24000;attributes=@{name='Previous draft';kind='unknown';fields=@{};edited_facets=@()}};region=$null;baseline=$null;saving=$null}
 $seed.workspaces.sources=@(@{source=$ref;total=$fixture.previous_capture.timeline.frame_count;active='previous-draft';view=@{capture=$fixture.previous_capture;viewport_start=0;viewport_end=48000};drafts=@($draft)})
 $seed.active_source=$ref;$seed.open=$false
}
$seed | ConvertTo-Json -Depth 60 | Set-Content "$state/sample-workspaces.json"
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SamplingWindow {
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
 [DllImport("user32.dll")]public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")]public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
 [DllImport("user32.dll")]public static extern bool ShowWindow(IntPtr h,int command);
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
}
'@
[SamplingWindow]::SetThreadDpiAwarenessContext([IntPtr](-4))|Out-Null
$script:sequence=0
$process=$null
$loopback=$null
function Read-Ui {
 try {
  $stream=[IO.File]::Open("$state/ui-status.txt",[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
  $reader=[IO.StreamReader]::new($stream)
  try{$text=$reader.ReadToEnd()}finally{$reader.Dispose()}
  $lines=$text -split '\r?\n';if(!$lines[0].StartsWith('READY ')){return $null}
  $ui=[pscustomobject]@{Ack=[uint64](($lines[0] -split ' ')[-1]);Controls=@{};Activations=@{};PlayerSelection=$null;Sample=$null;SongPosition=0.;SongDuration=0.;SongPaused=([int](($lines[0] -split ' ')[7]) -ne 0)}
  foreach($line in $lines){
   if($line.StartsWith('PLAYER_SELECTION ')){$ui.PlayerSelection=$line.Substring(17)|ConvertFrom-Json}
   elseif($line.StartsWith('SAMPLER ')){$ui.Sample=$line.Substring(8)|ConvertFrom-Json}
   elseif($line.StartsWith('FEATURES ')){$feature=$line -split ' ';$ui.SongPosition=[double]::Parse($feature[5],[Globalization.CultureInfo]::InvariantCulture);$ui.SongDuration=[double]::Parse($feature[6],[Globalization.CultureInfo]::InvariantCulture)}
   elseif($line -match '^ACTIVATION "([^"]+)" ([0-9]+)$'){$ui.Activations[$matches[1]]=[uint64]$matches[2]}
   elseif($line -match '^CONTROL "([^"]+)" ([^ ]+) ([^ ]+) ([^ ]+) ([^ ]+) ([01]) ([01]) ([0-9]+)$'){
    $ui.Controls[$matches[1]]=[pscustomobject]@{X=([double]::Parse($matches[2],[Globalization.CultureInfo]::InvariantCulture)+[double]::Parse($matches[4],[Globalization.CultureInfo]::InvariantCulture))/2;Y=([double]::Parse($matches[3],[Globalization.CultureInfo]::InvariantCulture)+[double]::Parse($matches[5],[Globalization.CultureInfo]::InvariantCulture))/2;Top=[double]::Parse($matches[3],[Globalization.CultureInfo]::InvariantCulture);Bottom=[double]::Parse($matches[5],[Globalization.CultureInfo]::InvariantCulture);Left=[double]::Parse($matches[2],[Globalization.CultureInfo]::InvariantCulture);Right=[double]::Parse($matches[4],[Globalization.CultureInfo]::InvariantCulture);Clicks=[uint64]$matches[8];Hovered=($matches[6] -eq "1");Active=($matches[7] -eq "1")}
   }
  }
  return $ui
 }catch [IO.IOException]{return $null}
}
function Wait-Ui([scriptblock]$Condition,[string]$Description,[int]$TimeoutSeconds=60){
 $until=(Get-Date).AddSeconds($TimeoutSeconds)
 while((Get-Date)-lt $until){$ui=Read-Ui;if($ui -and (& $Condition $ui)){return $ui};if($process.HasExited){throw "Player exited during $Description"};Start-Sleep -Milliseconds 20}
 throw "Timed out: $Description. State: $((Read-Ui).Sample | ConvertTo-Json -Compress -Depth 6)"
}
function Get-Ui { return (Wait-Ui {param($ui)$true} 'UI snapshot') }
function Send-Input([string]$Action){
 $script:sequence++
 [IO.File]::WriteAllText("$state/ui-command.txt","$script:sequence $Action`n",[Text.UTF8Encoding]::new($false))
 $null=Wait-Ui {param($ui)$ui.Ack -eq $script:sequence} 'input acknowledgement'
}
function Click([string]$Name){
 $ui=Wait-Ui {param($ui)$ui.Controls.ContainsKey($Name)} $Name
 $control=$ui.Controls[$Name];$before=$control.Clicks
 Send-Input ('MOVE '+$control.X.ToString([Globalization.CultureInfo]::InvariantCulture)+' '+$control.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
 $null=Wait-Ui {param($ui)$ui.Controls.ContainsKey($Name) -and $ui.Controls[$Name].Hovered} "hover $Name"
 Send-Input 'DOWN'
 try{$null=Wait-Ui {param($ui)$ui.Controls.ContainsKey($Name) -and $ui.Controls[$Name].Active} "press $Name"}finally{Send-Input 'UP'}
 $null=Wait-Ui {param($ui)$ui.Activations.ContainsKey($Name) -and $ui.Activations[$Name] -gt $before} "activate $Name"
}
function Capture([string]$Name){
 $process.Refresh();$handle=$process.MainWindowHandle
 [SamplingWindow]::ShowWindow($handle,9)|Out-Null
 [SamplingWindow]::SetForegroundWindow($handle)|Out-Null
 $rect=[SamplingWindow+Rect]::new();[SamplingWindow]::GetWindowRect($handle,[ref]$rect)|Out-Null
 $bitmap=[Drawing.Bitmap]::new($rect.Right-$rect.Left,$rect.Bottom-$rect.Top)
 $graphics=[Drawing.Graphics]::FromImage($bitmap);$dc=$graphics.GetHdc()
 try{[SamplingWindow]::PrintWindow($handle,$dc,2)|Out-Null}finally{$graphics.ReleaseHdc($dc)}
 try{$bitmap.Save("$state/$Name.png")}finally{$graphics.Dispose();$bitmap.Dispose()}
}
function Require-CleanReport {
 if($env:PRAPPY_LEAK_REPORT){
  if(!(Test-Path -LiteralPath $env:PRAPPY_LEAK_REPORT) -or (Get-Content -LiteralPath $env:PRAPPY_LEAK_REPORT -Raw) -notmatch '^PASS no CRT leaks\s*$'){throw 'Sampling UI failed its Debug CRT leak check'}
 }
}
try{
 $process=Start-Process "$app/build/$BuildVariant/PrappyPlayer.exe" -WindowStyle Hidden -PassThru -ArgumentList '--window-test',('"'+$state+'"')
 if($WorkflowOnly){
  Click 'list';Click 'library-0'
  $before=Wait-Ui {param($ui)$ui.SongPosition -gt .25 -and !$ui.SongPaused} 'playing song'
  if($RegisterSource){
   $null=Wait-Ui {param($ui)$ui.Sample.preparation -eq 'Song is not registered for editing'} 'read-only background lookup of unregistered song'
   $fixtureHash=(Get-FileHash -LiteralPath $fixture.mark_song -Algorithm SHA256).Hash.ToLowerInvariant()
   $fixtureHeaders=@{Authorization='Bearer '+(Get-Content -LiteralPath $fixture.token_file -Raw).Trim()}
   $unregistered=Invoke-RestMethod -Uri ($fixture.url+'/v1/resolve?sha256='+$fixtureHash) -Headers $fixtureHeaders
   if($unregistered.matches.Count -ne 0){throw 'Background preparation registered a source or fixture was already registered'}
  }else{$null=Wait-Ui {param($ui)$ui.Sample.preparation -match 'ready'} 'background source preparation'}
  Send-Input 'KEY CTRL 1';Send-Input 'KEY E 1';Send-Input 'KEY E 0';Send-Input 'KEY CTRL 0'
  $null=Wait-Ui {param($ui)$ui.PlayerSelection.active} 'Ctrl+E enters selection mode'
  Send-Input 'KEY ESC 1';Send-Input 'KEY ESC 0'
  $null=Wait-Ui {param($ui)!$ui.PlayerSelection.active} 'Escape cancels selection mode'
  $plot=(Get-Ui).Controls['seek']
  $x={param($fraction)($plot.Left+($plot.Right-$plot.Left)*$fraction).ToString([Globalization.CultureInfo]::InvariantCulture)}
  $y=$plot.Y.ToString([Globalization.CultureInfo]::InvariantCulture)
  Send-Input ('MOVE '+(& $x .1)+' '+$y);Send-Input 'RIGHT_DOWN';Send-Input 'RIGHT_UP'
  Click 'waveform-select'
  # The menu activates after this frame's waveform height was computed. Wait
  # for the selection controls to fit above transport before using its bounds.
  $layout=Wait-Ui {param($ui)$ui.PlayerSelection.active -and $ui.Controls.ContainsKey('waveform-confirm') -and $ui.Controls['waveform-confirm'].Bottom -lt $ui.Controls['play'].Top} 'selection layout above transport'
  $plot=$layout.Controls['seek'];$y=$plot.Y.ToString([Globalization.CultureInfo]::InvariantCulture)
  Send-Input ('MOVE '+(& $x .7)+' '+$y);Send-Input 'DOWN';Send-Input ('MOVE '+(& $x .4)+' '+$y);Send-Input 'UP'
  $song=Get-Ui
  if($song.SongPaused -or $song.SongPosition -gt 6){throw 'Selection gesture sought or paused the song'}
  # Edge adjustment, Escape rollback during a drag, and whole-range move.
  $range=$song.PlayerSelection
  Send-Input ('MOVE '+(& $x ($range.start/$song.SongDuration))+' '+$y);Send-Input 'DOWN';Send-Input ('MOVE '+(& $x .45)+' '+$y);Send-Input 'UP'
  $edge=Get-Ui
  if($edge.PlayerSelection.start -le $range.start -or [Math]::Abs($edge.PlayerSelection.end-$range.end)*48000 -gt 1){throw ('In handle did not adjust independently: before='+($range|ConvertTo-Json -Compress)+' after='+($edge.PlayerSelection|ConvertTo-Json -Compress))}
  Send-Input ('MOVE '+(& $x ($edge.PlayerSelection.end/$song.SongDuration))+' '+$y);Send-Input 'DOWN';Send-Input ('MOVE '+(& $x .8)+' '+$y)
  Send-Input 'KEY ESC 1';Send-Input 'KEY ESC 0';Send-Input 'UP'
  $rolled=Get-Ui
  if(!$rolled.PlayerSelection.active -or [Math]::Abs($rolled.PlayerSelection.end-$edge.PlayerSelection.end)*48000 -gt 1){throw ('Escape rollback: before='+($edge.PlayerSelection|ConvertTo-Json -Compress)+' after='+($rolled.PlayerSelection|ConvertTo-Json -Compress))}
  Send-Input ('MOVE '+(& $x .55)+' '+$y);Send-Input 'DOWN';Send-Input ('MOVE '+(& $x .6)+' '+$y);Send-Input 'UP'
  $song=Get-Ui
  if([Math]::Abs(($song.PlayerSelection.end-$song.PlayerSelection.start)-($edge.PlayerSelection.end-$edge.PlayerSelection.start)) -gt (1./48000)){throw ('Moving a range changed its duration: before='+($edge.PlayerSelection|ConvertTo-Json -Compress)+' after='+($song.PlayerSelection|ConvertTo-Json -Compress))}
  Capture 'player-selection'
  Click 'waveform-confirm'
  if($RegisterSource){
   $marked=Wait-Ui {param($ui)$ui.Sample.open -and !$ui.Sample.mark_pending -and $ui.Sample.capture -and $ui.Sample.capture -ne $fixture.previous_capture.id -and $ui.Sample.selection_defined} 'verified registration and exact mouse range handoff'
   $fixture.mark_capture_id=$marked.Sample.capture
   $registered=Invoke-RestMethod -Uri ($fixture.url+'/v1/resolve?sha256='+$fixtureHash) -Headers $fixtureHeaders
   if($registered.matches.Count -ne 1 -or $registered.matches[0].capture.id -ne $fixture.mark_capture_id){throw 'Source registration created duplicate or mismatched captures'}
   if((Get-FileHash -LiteralPath $fixture.mark_song -Algorithm SHA256).Hash.ToLowerInvariant() -ne $fixtureHash){throw 'Source registration changed the original recording'}
  }else{$marked=Wait-Ui {param($ui)$ui.Sample.open -and !$ui.Sample.mark_pending -and $ui.Sample.capture -eq $fixture.mark_capture_id -and $ui.Sample.selection_defined} 'mouse range handoff'}
  if($marked.Sample.start -ne [Math]::Round(48000*$song.PlayerSelection.start) -or $marked.Sample.end -ne [Math]::Round(48000*$song.PlayerSelection.end)){throw 'Handed-off range differs from player drag'}
  if($AudioLoopback){
   Add-Type -Path "$PSScriptRoot/AudioLoopback.cs"
   $loopback=[PrappyAudioLoopback]::new([double]$fixture.tone_hz)
   Start-Sleep -Milliseconds 200
   $baseline=$loopback.ToneAmplitude;$loopback.Clear()
  }
  Click 'sample-play'
  $null=Wait-Ui {param($ui)$ui.Sample.audio_frames_submitted -gt 4800 -and $ui.Sample.playhead -gt $ui.Sample.start+2400} 'selected audio plays' 5
  if($AudioLoopback){
   Start-Sleep -Milliseconds 700
   # Endpoint/session volume belongs to the user; test coherent tone presence relative to RMS, not loudness.
   $level=$loopback.ToneAmplitude
   if($loopback.Frames -lt 4800 -or $level -lt [Math]::Max(1e-8,$baseline*3) -or $loopback.Rms -eq 0 -or $level/$loopback.Rms -lt .75){throw "Expected selected tone absent from WASAPI output: baseline=$baseline playing=$level rms=$($loopback.Rms) frames=$($loopback.Frames)"}
   Write-Host "PASS WASAPI loopback selected $($fixture.tone_hz) Hz tone: baseline=$baseline output=$level frames=$($loopback.Frames)"
   $loopback.Dispose();$loopback=$null
  }
  Click 'sample-stop';Click 'sample-save'
  $saved=Wait-Ui {param($ui)$ui.Sample.region -and $ui.Sample.pending -eq 0} 'canonical region saved'
  Click 'sample-new'
  $two=Wait-Ui {param($ui)$ui.Sample.draft_count -eq 2 -and !$ui.Sample.region} 'second independent draft'
  $second=$two.Sample.active_draft
  Click 'sample-workspaces';Click ('sample-workspace-'+$fixture.previous_capture.id)
  $null=Wait-Ui {param($ui)$ui.Sample.capture -eq $fixture.previous_capture.id -and $ui.Sample.draft_count -eq 1} 'previous song draft retained'
  Click 'sample-workspaces';Click ('sample-workspace-'+$fixture.mark_capture_id)
  $null=Wait-Ui {param($ui)$ui.Sample.active_draft -eq $second -and $ui.Sample.draft_count -eq 2} 'same song restores both samples without duplicate source'
  Capture 'source-samples'
  $null=$process.CloseMainWindow();if(!$process.WaitForExit(10000)){throw 'Player did not close'}
  Remove-Item -LiteralPath "$state/ui-status.txt","$state/ui-command.txt"
  $script:sequence=0
  $process=Start-Process "$app/build/$BuildVariant/PrappyPlayer.exe" -WindowStyle Hidden -PassThru -ArgumentList '--window-test',('"'+$state+'"')
  $null=Wait-Ui {param($ui)$ui.Sample.open -and $ui.Sample.active_draft -eq $second -and $ui.Sample.draft_count -eq 2 -and $ui.Sample.workspace_count -eq 2} 'two source workspaces and many drafts survive restart'
  Write-Host "PASS mouse context menu, reverse drag without seek, exact editor handoff/play/save, many samples, canonical source switching and restart. Evidence: $state"
  return
 }
 if($MarkOnly){
  Click 'list'
  Click 'library-0'
  $before=Wait-Ui {param($ui)$ui.SongPosition -gt .25 -and !$ui.SongPaused} 'song playing in player mode'
  Send-Input 'KEY CTRL 1';Send-Input 'KEY M 1';Send-Input 'KEY M 0';Send-Input 'KEY CTRL 0'
  $marked=Wait-Ui {param($ui)$ui.Sample.open -and !$ui.Sample.mark_pending -and $ui.Sample.capture -eq $fixture.mark_capture_id} 'cold Ctrl+M loads current song across libraries'
  if(!$marked.SongPaused -or $marked.Sample.playhead -lt [Math]::Floor($before.SongPosition*48000)-100 -or $marked.Sample.playhead -gt [Math]::Ceiling(($before.SongPosition+1)*48000)){throw 'Mark did not capture playing song position'}
  Click 'sample-new'
  $marked=Wait-Ui {param($ui)$ui.Sample.selection_defined} 'new sample selected'
  Click 'sample-play'
  $playing=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Playing' -and $ui.Sample.audio_frames_submitted -gt 4800 -and $ui.Sample.playhead -gt $marked.Sample.start+2400} 'cold mark submits audio and advances selection playhead' 5
  if($playing.Sample.playback_scope -ne 0 -or $playing.Sample.playback_start -ne $marked.Sample.start -or $playing.Sample.playback_end -ne $marked.Sample.end){throw 'Marked playback is not the selected region'}
  Capture 'marked-current-song'
  Click 'sample-stop'
  Click 'sample-return';Click 'play'
  $null=Wait-Ui {param($ui)$ui.SongPosition -gt ($marked.Sample.start/48000.)+.3 -and !$ui.SongPaused} 'song resumes after editor return'
  Send-Input 'KEY CTRL 1';Send-Input 'KEY M 1';Send-Input 'KEY M 0';Send-Input 'KEY CTRL 0'
  $null=Wait-Ui {param($ui)!$ui.Sample.mark_pending -and $ui.Sample.capture -eq $fixture.mark_capture_id -and $ui.Sample.playhead -gt $marked.Sample.start+4800} 'warm Ctrl+M marks new position'
  Click 'sample-play'
  $null=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Playing' -and $ui.Sample.audio_frames_submitted -gt 4800 -and $ui.Sample.playhead -gt $ui.Sample.start+2400} 'warm mark submits audio and advances selection playhead' 5
  Write-Host "PASS cold/warm Ctrl+M and editor playback across libraries at the captured position, preserving prior draft. Evidence: $state"
  return
 }
 $null=Wait-Ui {param($ui)$ui.Sample.open} 'sampling workspace'
 if($StartupOnly){
  $null=Wait-Ui {param($ui)$ui.Sample.notice -eq 'Connected' -and $ui.Controls.ContainsKey('sample-item-0')} 'automatic backend connection'
  Write-Host "PASS automatic backend connection without configuration or Connect click. Evidence: $state"
  return
 }
 Click 'sample-item-0'
 Click 'sample-new'
 $selected=Wait-Ui {param($ui)$ui.Sample.waveform_exact_pcm} 'canonical PCM detail'
 if($RecordingOnly){
  Click 'sample-scope-recording';Click 'sample-loop';Click 'sample-follow';Click 'sample-play'
  $null=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Playing'} 'recording transport'
  $streamed=Wait-Ui {param($ui)$ui.Sample.playback_chunks -ge 2 -and $ui.Sample.playhead -gt 48000*30} 'recording crosses prefetched chunk boundary'
  if($streamed.Sample.viewport_start -le 0 -or $streamed.Sample.playhead -lt $streamed.Sample.viewport_start -or $streamed.Sample.playhead -gt $streamed.Sample.viewport_end){throw 'Follow playhead did not reveal recording position'}
  $null=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Stopped' -and $ui.Sample.playhead -eq $ui.Sample.playback_end} 'recording EOF drains and stops'
  Capture 'recording-eof'
  Click 'sample-stop';Click 'sample-play'
  $null=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Playing'} 'recording restart'
  Send-Input 'NATIVE_KEY SPACE 1';Send-Input 'NATIVE_KEY SPACE 0'
  $paused=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Paused'} 'Space pauses editor transport'
  $plot=(Get-Ui).Controls['sample-waveform-detail']
  Send-Input ('MOVE '+($plot.Left+($plot.Right-$plot.Left)*.8).ToString([Globalization.CultureInfo]::InvariantCulture)+' '+$plot.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
  Send-Input 'DOWN';Send-Input 'UP'
  $null=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Paused' -and $ui.Sample.playhead -gt $paused.Sample.playhead+48000} 'waveform click seeks paused recording'
  Send-Input 'NATIVE_KEY SPACE 1';Send-Input 'NATIVE_KEY SPACE 0'
  $null=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Playing'} 'Space resumes editor transport'
  Click 'sample-item-0'
  $null=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Stopped' -and $ui.Sample.playback_scope -eq 0} 'source switch stops audition'
  Write-Host "PASS long recording chunks, audible playhead, EOF, follow, Space, click seek and source-switch stop. Evidence: $state"
  return
 }
 $start=$selected.Sample.start
 Click 'sample-amplitude-fit'
 $initialFit=Wait-Ui {param($ui)$ui.Sample.amplitude_mode -eq 1 -and $ui.Sample.amplitude_gain -gt 1} 'visible amplitude fit'
 $overview=(Get-Ui).Controls['sample-waveform-overview']
 Send-Input ('MOVE '+($overview.Left+($overview.Right-$overview.Left)*.8).ToString([Globalization.CultureInfo]::InvariantCulture)+' '+$overview.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
 Send-Input 'DOUBLE_CLICK'
 $null=Wait-Ui {param($ui)$ui.Sample.amplitude_mode -eq 1 -and $ui.Sample.amplitude_gain -gt $initialFit.Sample.amplitude_gain*1.1} 'fit follows quieter visible passage'
 $null=Wait-Ui {param($ui)$ui.Sample.waveform_exact_pcm -and $ui.Sample.pcm_start -le [Math]::Floor($ui.Sample.viewport_start) -and $ui.Sample.pcm_end -ge [Math]::Ceiling($ui.Sample.viewport_end)} 'fractional navigation retains exact PCM detail'
 Click 'sample-amplitude-actual'
 $null=Wait-Ui {param($ui)$ui.Sample.amplitude_mode -eq 0 -and $ui.Sample.amplitude_gain -eq 1} 'actual amplitude reference'
 $ui=Wait-Ui {param($ui)$ui.Controls.ContainsKey('sample-amplitude-ruler')} 'amplitude ruler'
 $ruler=$ui.Controls['sample-amplitude-ruler']
 Send-Input ('MOVE '+$ruler.X.ToString([Globalization.CultureInfo]::InvariantCulture)+' '+$ruler.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
 Send-Input 'DOWN'
 $null=Wait-Ui {param($ui)$ui.Controls['sample-amplitude-ruler'].Active} 'ruler drag begins'
 Send-Input ('MOVE '+$ruler.X.ToString([Globalization.CultureInfo]::InvariantCulture)+' '+($ruler.Y-40).ToString([Globalization.CultureInfo]::InvariantCulture))
 Send-Input 'UP'
 $null=Wait-Ui {param($ui)$ui.Sample.amplitude_mode -eq 2 -and $ui.Sample.amplitude_gain -gt 1} 'manual amplitude drag'
 Send-Input 'DOUBLE_CLICK'
 $null=Wait-Ui {param($ui)$ui.Sample.amplitude_mode -eq 0 -and $ui.Sample.amplitude_gain -eq 1} 'ruler double-click resets actual scale'
 Click 'sample-amplitude-fit-now'
 $amplitude=Wait-Ui {param($ui)$ui.Sample.amplitude_mode -eq 2 -and $ui.Sample.amplitude_gain -gt 1} 'one-shot visible fit'
 Click 'sample-fit'
 $zoomed=Wait-Ui {param($ui)$ui.Sample.amplitude_mode -eq 2 -and [Math]::Abs($ui.Sample.amplitude_gain-$amplitude.Sample.amplitude_gain) -lt .000001 -and [Math]::Abs($ui.Sample.viewport_start-[Math]::Max(0,$ui.Sample.start-[Math]::Max(100,[Math]::Floor(($ui.Sample.end-$ui.Sample.start)/10)))) -lt 1} 'selection time zoom with unchanged amplitude'
 if($zoomed.Sample.start -ne $start -or $zoomed.Sample.end -ne $selected.Sample.end){throw 'Time zoom changed splice boundaries'}
 Send-Input ('MOVE '+($overview.Left+($overview.Right-$overview.Left)*.8).ToString([Globalization.CultureInfo]::InvariantCulture)+' '+$overview.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
 Send-Input 'DOUBLE_CLICK'
 $null=Wait-Ui {param($ui)[Math]::Abs($ui.Sample.viewport_start-$zoomed.Sample.viewport_start) -gt 100} 'navigate away from selection'
 Send-Input 'KEY F 1';Send-Input 'KEY F 0'
 $null=Wait-Ui {param($ui)[Math]::Abs($ui.Sample.viewport_start-$zoomed.Sample.viewport_start) -lt 1 -and [Math]::Abs($ui.Sample.viewport_end-$zoomed.Sample.viewport_end) -lt 1} 'F restores padded selection zoom'
 if($amplitude.Sample.start -ne $start -or $amplitude.Sample.end -ne $selected.Sample.end){throw 'Display amplitude changed splice boundaries'}
 Click 'sample-in-plus'
 $null=Wait-Ui {param($ui)$ui.Sample.start -eq $start+1} 'one-frame nudge'
 Click 'sample-undo'
 $null=Wait-Ui {param($ui)$ui.Sample.start -eq $start} 'undo'
 Click 'sample-redo'
 $null=Wait-Ui {param($ui)$ui.Sample.start -eq $start+1} 'redo'
 Click 'sample-save'
 $saved=Wait-Ui {param($ui)$ui.Sample.region -and $ui.Sample.pending -eq 0} 'saved sample'
 $regionId=$saved.Sample.region.id
 Click 'sample-play'
 $playing=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Playing' -and $ui.Sample.playback_loop -and $ui.Sample.playback_scope -eq 0} 'default looped selection transport'
 $null=Wait-Ui {param($ui)$ui.Sample.playhead -gt $playing.Sample.playhead+12000} 'audible playhead advances'
 Capture 'playing-selection'
 Click 'sample-play'
 $paused=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Paused'} 'pause audition'
 Send-Input 'MOVE 5 5'
 if((Read-Ui).Sample.playhead -ne $paused.Sample.playhead){throw 'Paused playhead moved'}
 Click 'sample-play'
 $null=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Playing'} 'resume audition'
 Click 'sample-in-plus'
 $null=Wait-Ui {param($ui)$ui.Sample.playback_start -eq $start+2 -and $ui.Sample.playback_state -eq 'Playing'} 'live boundary edit updates audible range'
 Click 'sample-undo'
 $null=Wait-Ui {param($ui)$ui.Sample.playback_start -eq $start+1 -and $ui.Sample.playback_state -eq 'Playing'} 'undo updates audition'
 Click 'sample-scope-context'
 $null=Wait-Ui {param($ui)$ui.Sample.playback_scope -eq 1 -and $ui.Sample.playback_start -eq 0 -and $ui.Sample.playback_state -eq 'Playing'} 'context audition'
 Click 'sample-scope-recording'
 $null=Wait-Ui {param($ui)$ui.Sample.playback_scope -eq 2 -and $ui.Sample.playback_end -gt $ui.Sample.end -and $ui.Sample.playback_state -eq 'Playing'} 'recording audition'
 Click 'sample-loop'
 $null=Wait-Ui {param($ui)!$ui.Sample.playback_loop -and $ui.Sample.playback_state -eq 'Playing'} 'live loop toggle'
 Click 'sample-scope-selection'
 $null=Wait-Ui {param($ui)$ui.Sample.playback_scope -eq 0 -and $ui.Sample.playback_state -eq 'Playing'} 'selection audition restored'
 Click 'sample-stop'
 $null=Wait-Ui {param($ui)$ui.Sample.playback_state -eq 'Stopped' -and $ui.Sample.playhead -eq $ui.Sample.start} 'stop resets to In'
 Click 'sample-loop'
 Click 'sample-start'
 $null=Wait-Ui {param($ui)$ui.Sample.playback_start -eq 0 -and $ui.Sample.playback_state -eq 'Playing'} 'boundary check'
 Click 'sample-stop'
 Click 'sample-classify'
 $null=Wait-Ui {param($ui)$ui.Sample.notice -eq 'Analysis ready: estimates are proposals.'} 'background beat detection'
 Capture 'workspace'
 Click 'sample-export'
 $null=Wait-Ui {param($ui)$ui.Sample.notice.StartsWith('Exported: ')} 'verified export'
 $null=$process.CloseMainWindow();if(!$process.WaitForExit(10000)){throw 'Player did not close'}
 Require-CleanReport
 $store=Get-Content "$state/sample-workspaces.json" -Raw | ConvertFrom-Json
 $ws=@($store.workspaces.sources | Where-Object {$_.source.capture_id -eq $store.active_source.capture_id})[0]
 $local=@($ws.drafts | Where-Object {$_.id -eq $ws.active})[0]
 $draft=[pscustomobject]@{region=$local.region;span=@{start=$local.value.start};amplitude_mode=$ws.view.amplitude_mode;amplitude_gain=$ws.view.amplitude_gain;gain=$store.gain;transport_loop=$ws.view.loop;transport_scope=$ws.view.scope;transport_follow=$ws.view.follow}
 if($draft.region.id -ne $regionId -or $draft.span.start -ne $start+1){throw 'Saved draft mismatch'}
 if($draft.amplitude_mode -ne 2 -or [Math]::Abs($draft.amplitude_gain-$amplitude.Sample.amplitude_gain) -gt .000001 -or [Math]::Abs($draft.gain-.7) -gt .000001){throw 'Amplitude persistence or independent audio gain failed'}
 if(!$draft.transport_loop -or $draft.transport_scope -ne 0 -or $draft.transport_follow){throw 'Editor transport preferences were not saved'}
 Remove-Item -LiteralPath "$state/ui-status.txt","$state/ui-command.txt"
 $script:sequence=0
 $process=Start-Process "$app/build/$BuildVariant/PrappyPlayer.exe" -WindowStyle Hidden -PassThru -ArgumentList '--window-test',('"'+$state+'"')
 $null=Wait-Ui {param($ui)$ui.Sample.region.id -eq $regionId -and $ui.Sample.start -eq $start+1} 'draft restored'
 $null=Wait-Ui {param($ui)$ui.Sample.pcm_frames -gt 0 -and $ui.Sample.region.id -eq $regionId} 'reconnected sample'
 $null=Wait-Ui {param($ui)$ui.Sample.amplitude_mode -eq 2 -and [Math]::Abs($ui.Sample.amplitude_gain-$amplitude.Sample.amplitude_gain) -lt .000001} 'amplitude restored'
 Write-Host "PASS sampling UI: browse, PCM, exact nudge, undo/redo, save, audition, background analysis, export and reopen. Evidence: $state"
}finally{
 if($loopback){$loopback.Dispose()}
 if($process -and !$process.HasExited){$null=$process.CloseMainWindow();if(!$process.WaitForExit(10000)){throw "Test player did not close: $($process.Id)"}}
 if($process -and $process.HasExited){Require-CleanReport}
}
