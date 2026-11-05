param([Parameter(Mandatory)][string]$Manifest,[string]$BuildVariant='Release',
    [ValidateRange(1,100)][int]$Iterations=1,[switch]$EvictPreparedPages)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$fixture=Get-Content -LiteralPath $Manifest -Raw|ConvertFrom-Json
$state=Join-Path $app ('build/prepared-editor-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory $state
$encoding=[Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText("$state/player.ini","directory=$state/unused`nextensions=wav`ndebug_log=true",$encoding)
[IO.File]::WriteAllText("$state/sampling-connection.json",(@{url=$fixture.url;token_file=$fixture.token_file;library=$fixture.library_id}|ConvertTo-Json),$encoding)
$script:sequence=0
$process=$null
function Read-Ui {
 try {
  $stream=[IO.File]::Open("$state/ui-status.txt",[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
  $reader=[IO.StreamReader]::new($stream)
  try{$text=$reader.ReadToEnd()}finally{$reader.Dispose()}
  $lines=$text -split '\r?\n'
  if(!$lines[0].StartsWith('READY ')){return $null}
  $ui=[pscustomobject]@{Ack=[uint64](($lines[0] -split ' ')[-1]);Width=[int](($lines[0]-split ' ')[1]);Height=[int](($lines[0]-split ' ')[2]);Controls=@{};Sample=$null;Catalog=$null;Duration=0.;Position=0.}
  foreach($line in $lines){
   if($line.StartsWith('SAMPLER ')){$ui.Sample=$line.Substring(8)|ConvertFrom-Json}
   elseif($line.StartsWith('CATALOG ')){$ui.Catalog=$line.Substring(8)|ConvertFrom-Json}
   elseif($line.StartsWith('FEATURES ')){$ui.Duration=[double](($line -split ' ')[6]);$ui.Position=[double](($line -split ' ')[5])}
   elseif($line -match '^CONTROL "([^"]+)" ([^ ]+) ([^ ]+) ([^ ]+) ([^ ]+) ([01]) ([01]) '){
    $ui.Controls[$matches[1]]=@{X=([double]$matches[2]+[double]$matches[4])/2;Y=([double]$matches[3]+[double]$matches[5])/2;Left=[double]$matches[2];Right=[double]$matches[4];Bottom=[double]$matches[5];Hovered=($matches[6]-eq '1');Active=($matches[7]-eq '1')}
   }
  }
  return $ui
 }catch [IO.IOException]{return $null}
}
function Wait-Ui([scriptblock]$Condition,[string]$Description,[int]$Timeout=15){
 $until=[datetime]::UtcNow.AddSeconds($Timeout)
 do {
  $ui=Read-Ui
  if($ui -and (& $Condition $ui)){return $ui}
  if($process.HasExited){throw "Player exited during $Description"}
  Start-Sleep -Milliseconds 20
 }while([datetime]::UtcNow-lt $until)
 throw "Timeout: $Description. $((Read-Ui).Sample|ConvertTo-Json -Compress)"
}
function Send-Input([string]$Action){
 $script:sequence++
 [IO.File]::WriteAllText("$state/ui-command.txt","$script:sequence $Action`n",$encoding)
 $null=Wait-Ui {param($ui)$ui.Ack-eq $script:sequence} "input $Action"
}
function Click([string]$Name){
 $ui=Wait-Ui {param($ui)$ui.Controls.ContainsKey($Name)} "control $Name"
 $control=$ui.Controls[$Name]
 Click-At $Name $control.X $control.Y
}
function Click-At([string]$Name,[double]$X,[double]$Y){
 Send-Input ('MOVE '+$X.ToString([Globalization.CultureInfo]::InvariantCulture)+' '+$Y.ToString([Globalization.CultureInfo]::InvariantCulture))
 $null=Wait-Ui {param($ui)$ui.Controls.ContainsKey($Name) -and $ui.Controls[$Name].Hovered} "hover $Name"
 Send-Input DOWN
 try{$null=Wait-Ui {param($ui)$ui.Controls.ContainsKey($Name) -and $ui.Controls[$Name].Active} "press $Name"}finally{Send-Input UP}
}
function Start-Player {
 $script:process=Start-Process "$app/build/$BuildVariant/PrappyPlayer.exe" -ArgumentList '--window-test',('"'+$state+'"') -WindowStyle Hidden -PassThru
}
function Stop-Player {
 if($process -and !$process.HasExited){$null=$process.CloseMainWindow();if(!$process.WaitForExit(5000)){Stop-Process -Id $process.Id}}
}
try {
 Start-Player
 $null=Wait-Ui {param($ui)$ui.Catalog.sources-eq 2} 'prepared catalog'
 Click list;Click library-0
 $song=Wait-Ui {param($ui)$ui.Duration-gt 14.9} 'prepared listening playback'
 $playerWidth=$song.Width;$playerHeight=$song.Height
 $headers=if($EvictPreparedPages){@{Authorization='Bearer '+[IO.File]::ReadAllText($fixture.token_file).Trim()}}else{@{}}
 if($EvictPreparedPages){$null=Invoke-RestMethod -Method Post -Headers $headers -Uri ($fixture.gate_url+'/evict?asset='+$fixture.mark_capture.timeline.working_asset_key)}
 $clock=[Diagnostics.Stopwatch]::StartNew()
 Send-Input 'KEY CTRL 1';Send-Input 'KEY M 1';Send-Input 'KEY M 0';Send-Input 'KEY CTRL 0'
 $ready=Wait-Ui {param($ui)$ui.Width-ge 1100 -and $ui.Height-ge 850 -and $ui.Sample.capture -and $ui.Sample.waveform_exact_pcm -and !$ui.Sample.mark_pending} 'native sample detail'
 $openMs=$clock.ElapsedMilliseconds
 $measurements=[Collections.Generic.List[long]]::new()
 $measurements.Add($openMs)
 $rate=[long]$fixture.mark_capture.timeline.sample_rate
 if($ready.Sample.end-ne $rate*4 -or $ready.Catalog.source_hash_reads-ne 0 -or $ready.Sample.notice.Contains('exception')){throw 'Native editor source/rate or waveform failed.'}
 $random=[Random]::new(43)
 for($iteration=1;$iteration-lt $Iterations;$iteration++){
  Click sample-return
  $song=Wait-Ui {param($ui)!$ui.Sample.open -and $ui.Width-eq $playerWidth -and $ui.Height-eq $playerHeight -and $ui.Controls.ContainsKey('seek') -and $ui.Controls['seek'].Right-le $ui.Width -and $ui.Controls['seek'].Bottom-le $ui.Height} 'settled player resize'
  $seek=$song.Controls['seek'];$fraction=.1+$random.NextDouble()*.8
  $x=$seek.Left+($seek.Right-$seek.Left)*$fraction
  Click-At seek $x $seek.Y
  $null=Wait-Ui {param($ui)[Math]::Abs($ui.Position-$fraction*$ui.Duration)-lt $ui.Duration/($seek.Right-$seek.Left)*2} 'random prepared seek'
  if($EvictPreparedPages){$null=Invoke-RestMethod -Method Post -Headers $headers -Uri ($fixture.gate_url+'/evict?asset='+$fixture.mark_capture.timeline.working_asset_key)}
  $clock.Restart()
  Send-Input 'KEY CTRL 1';Send-Input 'KEY M 1';Send-Input 'KEY M 0';Send-Input 'KEY CTRL 0'
  $ready=Wait-Ui {param($ui)$ui.Width-ge 1100 -and $ui.Height-ge 850 -and $ui.Sample.capture -and $ui.Sample.waveform_exact_pcm -and !$ui.Sample.mark_pending} 'random-position native detail'
  $measurements.Add($clock.ElapsedMilliseconds)
 }
 Click sample-listen
 $playing=Wait-Ui {param($ui)$ui.Sample.playback_chunks-ge 2 -and $ui.Sample.audio_frames_submitted-gt 0} 'prepared listening audition and prefetch'
 if($playing.Sample.playhead-lt 0 -or $playing.Sample.playhead-gt $fixture.mark_capture.timeline.frame_count){throw 'Audition cursor left the native timeline.'}
 Stop-Player
 $script:sequence=0
 Start-Player
 $restored=Wait-Ui {param($ui)$ui.Sample.capture-eq $ready.Sample.capture -and $ui.Sample.waveform_exact_pcm} 'native editor restart'
 if($restored.Sample.end-ne $ready.Sample.end){throw 'Editor restart changed native selection frames.'}
 $log=Get-Content "$state/player-debug.log" -Raw
 if($log-match 'tag=(hash|resolve|source-import|pcm|audition) '){throw 'Editor used retired source registration or raw PCM paths.'}
 $ordered=@($measurements|Sort-Object)
 $percentiles=@(.5,.95,.99)|ForEach-Object {$ordered[[Math]::Ceiling($_*$ordered.Count)-1]}
 [pscustomobject]@{result='PASS';open_ms=$openMs;open_ms_p50_p95_p99=$percentiles;iterations=$Iterations;advisory_eviction=[bool]$EvictPreparedPages;sample_rate=$rate;capture=$ready.Sample.capture;audition_chunks=$playing.Sample.playback_chunks;state=$state}|ConvertTo-Json
}finally{Stop-Player}
