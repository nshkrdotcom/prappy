param([Parameter(Mandatory)][string]$Manifest,[string]$BuildVariant='Release')
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
  $ui=[pscustomobject]@{Ack=[uint64](($lines[0] -split ' ')[-1]);Controls=@{};Sample=$null;Catalog=$null;Duration=0.}
  foreach($line in $lines){
   if($line.StartsWith('SAMPLER ')){$ui.Sample=$line.Substring(8)|ConvertFrom-Json}
   elseif($line.StartsWith('CATALOG ')){$ui.Catalog=$line.Substring(8)|ConvertFrom-Json}
   elseif($line.StartsWith('FEATURES ')){$ui.Duration=[double](($line -split ' ')[6])}
   elseif($line -match '^CONTROL "([^"]+)" ([^ ]+) ([^ ]+) ([^ ]+) ([^ ]+) '){
    $ui.Controls[$matches[1]]=@{X=([double]$matches[2]+[double]$matches[4])/2;Y=([double]$matches[3]+[double]$matches[5])/2}
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
 Send-Input ('MOVE '+$control.X.ToString([Globalization.CultureInfo]::InvariantCulture)+' '+$control.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
 Send-Input DOWN;Send-Input UP
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
 $null=Wait-Ui {param($ui)$ui.Duration-gt 14.9} 'prepared listening playback'
 $clock=[Diagnostics.Stopwatch]::StartNew()
 Send-Input 'KEY CTRL 1';Send-Input 'KEY M 1';Send-Input 'KEY M 0';Send-Input 'KEY CTRL 0'
 $ready=Wait-Ui {param($ui)$ui.Sample.capture -and $ui.Sample.waveform_exact_pcm -and !$ui.Sample.mark_pending} 'native sample detail'
 $openMs=$clock.ElapsedMilliseconds
 $rate=[long]$fixture.previous_capture.timeline.sample_rate
 if($ready.Sample.end-ne $rate*4 -or $ready.Catalog.source_hash_reads-ne 0 -or $ready.Sample.notice.Contains('exception')){throw 'Native editor source/rate or waveform failed.'}
 Click sample-listen
 $playing=Wait-Ui {param($ui)$ui.Sample.playback_chunks-ge 2 -and $ui.Sample.audio_frames_submitted-gt 0} 'prepared listening audition and prefetch'
 if($playing.Sample.playhead-lt 0 -or $playing.Sample.playhead-gt $fixture.previous_capture.timeline.frame_count){throw 'Audition cursor left the native timeline.'}
 Stop-Player
 $script:sequence=0
 Start-Player
 $restored=Wait-Ui {param($ui)$ui.Sample.capture-eq $ready.Sample.capture -and $ui.Sample.waveform_exact_pcm} 'native editor restart'
 if($restored.Sample.end-ne $ready.Sample.end){throw 'Editor restart changed native selection frames.'}
 $log=Get-Content "$state/player-debug.log" -Raw
 if($log-match 'tag=(hash|resolve|source-import|pcm|audition) '){throw 'Editor used retired source registration or raw PCM paths.'}
 [pscustomobject]@{result='PASS';open_ms=$openMs;sample_rate=$rate;capture=$ready.Sample.capture;audition_chunks=$playing.Sample.playback_chunks;state=$state}|ConvertTo-Json
}finally{Stop-Player}
