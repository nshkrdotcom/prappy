#Requires -Version 7.0
param([Parameter(Mandatory)][string]$Manifest,[string]$BuildVariant='Release',
    [switch]$StartupOnly,[switch]$WorkstationDefaults)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$fixture=Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
$state=Join-Path $app ('build/sampling-ui-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$state/media" | Out-Null
"directory=$state/media`nextensions=wav" | Set-Content "$state/player.ini"
if(!$WorkstationDefaults){@{url=$fixture.url;token_file=$fixture.token_file;library=$fixture.library_id} | ConvertTo-Json | Set-Content "$state/sampling-connection.json"}
$initialLibrary=if($WorkstationDefaults){''}else{$fixture.library_id}
@{capture=$null;region=$null;name='UI sample';kind='unknown';fields=@{};edited_facets=@();pending=@();library=$initialLibrary;open=$true} | ConvertTo-Json -Depth 5 | Set-Content "$state/sample-draft.json"
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
function Read-Ui {
 try {
  $stream=[IO.File]::Open("$state/ui-status.txt",[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
  $reader=[IO.StreamReader]::new($stream)
  try{$text=$reader.ReadToEnd()}finally{$reader.Dispose()}
  $lines=$text -split '\r?\n';if(!$lines[0].StartsWith('READY ')){return $null}
  $ui=[pscustomobject]@{Ack=[uint64](($lines[0] -split ' ')[-1]);Controls=@{};Activations=@{};Sample=$null}
  foreach($line in $lines){
   if($line.StartsWith('SAMPLER ')){$ui.Sample=$line.Substring(8)|ConvertFrom-Json}
   elseif($line -match '^ACTIVATION "([^"]+)" ([0-9]+)$'){$ui.Activations[$matches[1]]=[uint64]$matches[2]}
   elseif($line -match '^CONTROL "([^"]+)" ([^ ]+) ([^ ]+) ([^ ]+) ([^ ]+) ([01]) ([01]) ([0-9]+)$'){
    $ui.Controls[$matches[1]]=[pscustomobject]@{X=([double]::Parse($matches[2],[Globalization.CultureInfo]::InvariantCulture)+[double]::Parse($matches[4],[Globalization.CultureInfo]::InvariantCulture))/2;Y=([double]::Parse($matches[3],[Globalization.CultureInfo]::InvariantCulture)+[double]::Parse($matches[5],[Globalization.CultureInfo]::InvariantCulture))/2;Clicks=[uint64]$matches[8];Hovered=($matches[6] -eq "1");Active=($matches[7] -eq "1")}
   }
  }
  return $ui
 }catch [IO.IOException]{return $null}
}
function Wait-Ui([scriptblock]$Condition,[string]$Description){
 $until=(Get-Date).AddSeconds(60)
 while((Get-Date)-lt $until){$ui=Read-Ui;if($ui -and (& $Condition $ui)){return $ui};if($process.HasExited){throw "Player exited during $Description"};Start-Sleep -Milliseconds 20}
 throw "Timed out: $Description. State: $((Read-Ui).Sample | ConvertTo-Json -Compress -Depth 6)"
}
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
 $null=Wait-Ui {param($ui)$ui.Sample.open} 'sampling workspace'
 if($StartupOnly){
  $null=Wait-Ui {param($ui)$ui.Sample.notice -eq 'Connected' -and $ui.Controls.ContainsKey('sample-item-0')} 'automatic backend connection'
  Write-Host "PASS automatic backend connection without configuration or Connect click. Evidence: $state"
  return
 }
 Click 'sample-item-0'
 $selected=Wait-Ui {param($ui)$ui.Sample.pcm_frames -gt 0} 'canonical PCM'
 $start=$selected.Sample.start
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
 $null=Wait-Ui {param($ui)$ui.Sample.notice -eq 'Auditioning canonical PCM'} 'canonical audition'
 Click 'sample-stop'
 Click 'sample-classify'
 $null=Wait-Ui {param($ui)$ui.Sample.notice -eq 'Analysis ready: estimates are proposals.'} 'background beat detection'
 Capture 'workspace'
 Click 'sample-export'
 $null=Wait-Ui {param($ui)$ui.Sample.notice.StartsWith('Exported: ')} 'verified export'
 $null=$process.CloseMainWindow();if(!$process.WaitForExit(10000)){throw 'Player did not close'}
 Require-CleanReport
 $draft=Get-Content "$state/sample-draft.json" -Raw | ConvertFrom-Json
 if($draft.region.id -ne $regionId -or $draft.span.start -ne $start+1){throw 'Saved draft mismatch'}
 Remove-Item -LiteralPath "$state/ui-status.txt","$state/ui-command.txt"
 $script:sequence=0
 $process=Start-Process "$app/build/$BuildVariant/PrappyPlayer.exe" -WindowStyle Hidden -PassThru -ArgumentList '--window-test',('"'+$state+'"')
 $null=Wait-Ui {param($ui)$ui.Sample.region.id -eq $regionId -and $ui.Sample.start -eq $start+1} 'draft restored'
 $null=Wait-Ui {param($ui)$ui.Sample.pcm_frames -gt 0 -and $ui.Sample.region.id -eq $regionId} 'reconnected sample'
 Write-Host "PASS sampling UI: browse, PCM, exact nudge, undo/redo, save, audition, background analysis, export and reopen. Evidence: $state"
}finally{
 if($process -and !$process.HasExited){$null=$process.CloseMainWindow();if(!$process.WaitForExit(10000)){throw "Test player did not close: $($process.Id)"}}
 if($process -and $process.HasExited){Require-CleanReport}
}
