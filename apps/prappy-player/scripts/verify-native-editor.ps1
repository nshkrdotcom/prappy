param([Parameter(Mandatory)][string]$Manifest,[string]$BuildVariant='Release',
    [ValidateRange(1,100)][int]$Iterations=1,[switch]$EvictSourcePages,[switch]$InjectAnalysisPending,[switch]$InjectCatalogOffline,[switch]$VerifySaveExport,[switch]$VerifyCommands)
$ErrorActionPreference='Stop'
if(!('EditorMemoryNative' -as [type])){Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class EditorMemoryNative {
 [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process,uint kind);
}
'@}
$app=Split-Path $PSScriptRoot -Parent
$fixture=Get-Content -LiteralPath $Manifest -Raw|ConvertFrom-Json
if($VerifyCommands -and (!$VerifySaveExport -or !$fixture.generated_codec -or $fixture.url -match ':8765(/|$)')){throw 'Command/retry injection requires generated fixture media and save/export verification.'}
$stateDirectory=Join-Path $app ('build/native-editor-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory $stateDirectory
$encoding=[Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText("$stateDirectory/player.ini","debug_log=true",$encoding)
[IO.File]::WriteAllText("$stateDirectory/sampling-connection.json",(@{url=$fixture.url;token_file=$fixture.token_file;library=$fixture.library_id}|ConvertTo-Json),$encoding)
$script:inputSequence=0
$process=$null
. "$PSScriptRoot/ui-test-helpers.ps1"
$hwnd=[IntPtr]::Zero
function Start-Player {
 foreach($name in @('ui-status.txt','ui-command.txt')){Remove-Item -LiteralPath (Join-Path $stateDirectory $name) -ErrorAction SilentlyContinue}
 $script:process=Start-Process "$app/build/$BuildVariant/PrappyPlayer.exe" -ArgumentList '--window-test',('"'+$stateDirectory+'"') -WindowStyle Hidden -PassThru
}
function Scroll-Editor([int]$Amount){
 $ui=Read-Ui
 $name=@('sample-export','sample-save','sample-redo','sample-in-plus','sample-follow','sample-play')|Where-Object {$ui.Controls.ContainsKey($_) -and $ui.Controls[$_].Top-ge 40 -and $ui.Controls[$_].Bottom-le $ui.Height-16}|Select-Object -First 1
 if(!$name){throw 'No visible editor control for scrolling.'}
 $anchor=$ui.Controls[$name]
 Send-UiInput ('MOVE '+[int]$anchor.X+' '+[int]$anchor.Y)
 Send-UiInput ('WHEEL '+$Amount)
}
function Stop-Player {
 if($process -and !$process.HasExited){$null=$process.CloseMainWindow();if(!$process.WaitForExit(5000)){Stop-Process -Id $process.Id}}
}
function Measure-EditorResources($ui){
 $process.Refresh()
 if($ui.Sample.pcm_bytes-gt $ui.Sample.pcm_budget_bytes){throw 'Editor exceeded its accounted PCM budget.'}
 $resources.Add([pscustomobject]@{private_bytes=$process.PrivateMemorySize64;kernel_handles=$process.HandleCount;gdi_handles=[EditorMemoryNative]::GetGuiResources($process.Handle,0);user_handles=[EditorMemoryNative]::GetGuiResources($process.Handle,1);pcm_bytes=$ui.Sample.pcm_bytes;pcm_budget_bytes=$ui.Sample.pcm_budget_bytes})
}
try {
 if($InjectCatalogOffline){$fixtureHeaders=@{Authorization='Bearer '+[IO.File]::ReadAllText($fixture.token_file).Trim()};$null=Invoke-RestMethod -Method Post -Headers $fixtureHeaders -Uri ($fixture.gate_url+'/catalog-offline')}
 Start-Player
 $null=Wait-Ui {param($ui)$ui.Catalog.sources-eq $fixture.expected_sources} 'native catalog'
 $process.Refresh();$script:hwnd=$process.MainWindowHandle
 Click list;Click library-0
 $song=Wait-Ui {param($ui)$ui.Duration-gt 14.9} 'native source playback'
 $playerWidth=$song.Width;$playerHeight=$song.Height
 $headers=if($EvictSourcePages -or $InjectAnalysisPending -or $VerifyCommands){@{Authorization='Bearer '+[IO.File]::ReadAllText($fixture.token_file).Trim()}}else{@{}}
 if($InjectAnalysisPending){$null=Invoke-RestMethod -Method Post -Headers $headers -Uri ($fixture.gate_url+'/analysis-pending?count=2')}
 if($EvictSourcePages){$null=Invoke-RestMethod -Method Post -Headers $headers -Uri ($fixture.gate_url+'/evict?capture='+$fixture.capture_id)}
 $clock=[Diagnostics.Stopwatch]::StartNew()
 Send-UiInput 'KEY CTRL 1';Send-UiInput 'KEY M 1';Send-UiInput 'KEY M 0';Send-UiInput 'KEY CTRL 0'
 $ready=Wait-Ui {param($ui)$ui.Width-ge 1100 -and $ui.Height-ge 850 -and $ui.Sample.capture -and ($ui.Sample.pcm_selection_ready -and $ui.Sample.waveform_detail_bins -gt 0 -and $ui.Sample.waveform_stride -gt 0 -and $ui.Sample.waveform_stride -lt 4096) -and !$ui.Sample.mark_pending} 'native sample detail'
 $openMs=$clock.ElapsedMilliseconds
 $measurements=[Collections.Generic.List[long]]::new()
 $resources=[Collections.Generic.List[object]]::new()
 Measure-EditorResources $ready
 $measurements.Add($openMs)
 $rate=[long]$fixture.mark_capture.timeline.sample_rate
 if(($ready.Sample.end-$ready.Sample.start)-ne $rate*4 -or $ready.Sample.notice.Contains('exception')){throw 'Native editor source/rate or waveform failed.'}
 $random=[Random]::new(43)
 for($iteration=1;$iteration-lt $Iterations;$iteration++){
  Click sample-return
  $song=Wait-Ui {param($ui)!$ui.Sample.open -and $ui.Width-eq $playerWidth -and $ui.Height-eq $playerHeight -and $ui.Controls.ContainsKey('seek') -and $ui.Controls['seek'].Right-le $ui.Width -and $ui.Controls['seek'].Bottom-le $ui.Height} 'settled player resize'
  $seek=$song.Controls['seek'];$fraction=.1+$random.NextDouble()*.8
  $x=$seek.Left+($seek.Right-$seek.Left)*$fraction
  Click-At seek $x $seek.Y
  $null=Wait-Ui {param($ui)[Math]::Abs($ui.Position-$fraction*$ui.Duration)-lt $ui.Duration/($seek.Right-$seek.Left)*2} 'random native seek'
  if($EvictSourcePages){$null=Invoke-RestMethod -Method Post -Headers $headers -Uri ($fixture.gate_url+'/evict?capture='+$fixture.capture_id)}
  $clock.Restart()
  Send-UiInput 'KEY CTRL 1';Send-UiInput 'KEY M 1';Send-UiInput 'KEY M 0';Send-UiInput 'KEY CTRL 0'
  $ready=Wait-Ui {param($ui)$ui.Width-ge 1100 -and $ui.Height-ge 850 -and $ui.Sample.capture -and ($ui.Sample.pcm_selection_ready -and $ui.Sample.waveform_detail_bins -gt 0 -and $ui.Sample.waveform_stride -gt 0 -and $ui.Sample.waveform_stride -lt 4096) -and !$ui.Sample.mark_pending} 'random-position native detail'
  $measurements.Add($clock.ElapsedMilliseconds)
  Measure-EditorResources $ready
 }
 if($VerifyCommands){
  Click sample-classify
  $null=Wait-Ui {param($ui)$ui.Sample.notice.StartsWith('Analysis queued;') -and $ui.Sample.pending-eq 0} 'editor generic classify command'
 }
 if($VerifySaveExport){
  Click sample-new
  $draft=Wait-Ui {param($ui)$ui.Sample.active_draft -and $ui.Sample.selection_defined} 'explicit sample draft'
  Click sample-in-plus
  $null=Wait-Ui {param($ui)$ui.Sample.start-eq $draft.Sample.start+1} 'native one-frame trim'
  Click sample-undo
  $null=Wait-Ui {param($ui)$ui.Sample.start-eq $draft.Sample.start} 'native trim undo'
  Click sample-redo
  $null=Wait-Ui {param($ui)$ui.Sample.start-eq $draft.Sample.start+1} 'native trim redo'
  Scroll-Editor -8
  if($VerifyCommands){$null=Invoke-RestMethod -Method Post -Headers $headers -Uri ($fixture.gate_url+'/lose-save-reply')}
  Click sample-save
  if($VerifyCommands){
   $null=Wait-Ui {param($ui)$ui.Sample.pending-eq 1 -and $ui.Sample.notice.Contains('Injected committed-save response loss')} 'committed save with lost response'
   Scroll-Editor -16
   Click sample-retry
  }
  $saved=Wait-Ui {param($ui)$ui.Sample.region.id -and $ui.Sample.pending-eq 0} 'native region save acknowledgment'
  if($saved.Sample.region.timeline_id-ne $fixture.mark_capture.timeline_id){throw 'Saved region changed native timeline.'}
  if($saved.Sample.region.span.start_frame-ne $saved.Sample.start -or $saved.Sample.region.span.end_frame-ne $saved.Sample.end){throw 'Saved region changed native boundaries.'}
  Scroll-Editor -8
  Click sample-export
  $null=Wait-Ui {param($ui)$ui.Sample.notice.StartsWith('Exported: ')} 'verified native export' 30
  $exports=@(Get-ChildItem -LiteralPath "$stateDirectory/exports" -File)
  $extension=if($fixture.mark_capture.timeline.sample_format-eq 'float64'){'.w64'}else{'.wv'}
  if($exports.Count-ne 1 -or $exports[0].Length-le 0 -or $exports[0].Extension-ne $extension){throw 'Native export file/precision format is incorrect.'}
  $ready=$saved
  Scroll-Editor 8
 }
 Click sample-listen
 $playing=Wait-Ui {param($ui)$ui.Sample.playback_chunks-ge 2 -and $ui.Sample.audio_frames_submitted-gt 0} 'shared native audition and prefetch'
 if($playing.Sample.playhead-lt 0 -or $playing.Sample.playhead-gt $fixture.mark_capture.timeline.frame_count){throw 'Audition cursor left the native timeline.'}
 Stop-Player
 $script:inputSequence=0
 Start-Player
 $restored=Wait-Ui {param($ui)$ui.Sample.capture-eq $ready.Sample.capture -and $ui.Sample.pcm_selection_ready -and $ui.Sample.waveform_detail_bins-gt 0} 'native editor restart'
 if($restored.Sample.end-ne $ready.Sample.end){throw 'Editor restart changed native selection frames.'}
 $log=Get-Content "$stateDirectory/player-debug.log" -Raw
 if($InjectCatalogOffline -and ($log-notmatch 'play.waiting ' -or $log-notmatch 'play.available ')){throw 'Pending offline playback was not exercised.'}
 if($log-match 'tag=(hash|resolve|source-import|pcm|audition) '){throw 'Editor used retired source registration or raw PCM paths.'}
 $ordered=@($measurements|Sort-Object)
 $percentiles=@(.5,.95,.99)|ForEach-Object {$ordered[[Math]::Ceiling($_*$ordered.Count)-1]}
 [pscustomobject]@{result='PASS';open_ms=$openMs;open_ms_p50_p95_p99=$percentiles;iterations=$Iterations;advisory_eviction=[bool]$EvictSourcePages;commands_verified=[bool]$VerifyCommands;lost_save_reply_recovered=[bool]$VerifyCommands;sample_rate=$rate;capture=$ready.Sample.capture;audition_chunks=$playing.Sample.playback_chunks;state=$stateDirectory;resource_samples=$resources.ToArray()}|ConvertTo-Json -Depth 4
}finally{Stop-Player}
