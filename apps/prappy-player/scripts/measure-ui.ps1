#Requires -Version 7.0
param([string]$BuildVariant='Release-Unicode',[string]$StateSource='', [ValidateRange(2,30)][int]$Seconds=4)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
if (!$StateSource) { $StateSource=Join-Path $app 'build/Release' }
$code=Get-Content "$PSScriptRoot/verify-features.ps1" -Raw
$start=$code.IndexOf('Add-Type -AssemblyName'); $end=$code.IndexOf('$process = Start-Process')
. ([scriptblock]::Create($code.Substring($start,$end-$start)))
$start=$code.IndexOf('function Require('); $end=$code.IndexOf("`ntry {`n    `$timer=")
if ($end -lt $start) { throw 'Cannot find UI verification helpers.' }
. ([scriptblock]::Create($code.Substring($start,$end-$start)))
$results=@()
function Percentile($values,[double]$p) {
    $sorted=@($values | Sort-Object); if (!$sorted.Count) { return $null }
    return [Math]::Round($sorted[[Math]::Min($sorted.Count-1,[int][Math]::Floor(($sorted.Count-1)*$p))],3)
}
foreach($driver in @('software','direct3d11')) {
    $stateDirectory=Join-Path $app ('build/perf-'+$driver+'-'+[Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force "$stateDirectory/player-analysis" | Out-Null
    foreach($name in @('player.ini','player-catalog.bin','player-history.txt')) { Copy-Item -LiteralPath (Join-Path $StateSource $name) -Destination $stateDirectory }
    foreach($file in Get-ChildItem (Join-Path $StateSource 'player-analysis') -Filter '*.bin') {
        New-Item -ItemType HardLink -Path (Join-Path "$stateDirectory/player-analysis" $file.Name) -Target $file.FullName | Out-Null
    }
    $script:inputSequence=0; $hwnd=[IntPtr]::Zero
    if ($driver -eq 'software') { $env:PRAPPY_TEST_SOFTWARE='1' } else { Remove-Item Env:PRAPPY_TEST_SOFTWARE -ErrorAction SilentlyContinue }
    $process=Start-Process "$app/build/$BuildVariant/PrappyPlayer.exe" -WindowStyle Hidden -PassThru -ArgumentList '--window-test',('"{0}"' -f $stateDirectory)
    try {
        $null=Wait-Ui {param($s) $s.Duration -gt 1 -and $s.Waveform -gt 0 -and !$s.Pending} 'benchmark waveform restored'
        $process.Refresh(); $hwnd=$process.MainWindowHandle
        $null=Wait-Ui {param($s) $s.Renderer -eq $driver} 'requested renderer active'
        foreach($scene in @('compact','4k-list','4k-scroll','4k-waveform')) {
            if($scene -eq 'compact') { [FeatureNative]::SetWindowPos($hwnd,[IntPtr]::Zero,30,30,522,306,0x14) | Out-Null }
            if($scene -eq '4k-list') {
                Click 'list'
                [FeatureNative]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,3862,2216,0x14) | Out-Null
                $null=Wait-Ui {param($s) $s.Width -ge 3800 -and $s.Visible -gt 100} '4K expanded library ready'
            }
            if($scene -eq '4k-waveform') {
                $grip=Move-Control 'transport-resize'; Send-UiInput 'DOWN'
                Send-UiInput ('MOVE {0} {1}' -f $grip.X.ToString([Globalization.CultureInfo]::InvariantCulture),($grip.Y-500).ToString([Globalization.CultureInfo]::InvariantCulture))
                Send-UiInput 'UP'; Send-UiInput 'MOVE 0 0'
            }
            if((Read-Ui).Paused) { Click 'play' }
            Send-UiInput 'MOVE 0 0'; Start-Sleep -Milliseconds 500
            $first=Read-Ui; $samples=@(); $seen=[uint64]0; $timer=[Diagnostics.Stopwatch]::StartNew()
            while($timer.Elapsed.TotalSeconds -lt $Seconds) {
                if($scene -eq '4k-scroll') { Send-UiInput 'MOVE 250 300'; Send-UiInput 'WHEEL -2' }
                $snapshot=Read-Ui
                if($snapshot.Presents -ne $seen) { $samples+=$snapshot; $seen=$snapshot.Presents }
                Start-Sleep -Milliseconds 5
            }
            $last=Read-Ui
            $results += [pscustomobject]@{Renderer=$driver;Scene=$scene;Width=$last.Width;Height=$last.Height;Rows=$last.DrawnTracks;
                Fps=[Math]::Round(($last.Presents-$first.Presents)/$timer.Elapsed.TotalSeconds,1);
                FrameP50=Percentile @($samples | ForEach-Object FrameTotal) .5;FrameP95=Percentile @($samples | ForEach-Object FrameTotal) .95;
                BuildP95=Percentile @($samples | ForEach-Object FrameBuild) .95;DrawP95=Percentile @($samples | ForEach-Object FrameDraw) .95;
                PresentP95=Percentile @($samples | ForEach-Object FramePresent) .95;
                Shapes=$last.TextShaped-$first.TextShaped;Tiles=$last.TextRasterized-$first.TextRasterized;TextMiB=[Math]::Round($last.TextBytes/1MB,1)}
            if (!(Read-Ui).Paused) { Click 'play' }
            $inputTimes=@()
            foreach($ratio in @(.15,.8,.4,.65,.25,.9,.5,.33,.7,.1)) {
                $seek=(Read-Ui).Controls['seek']; $x=$seek.Left+($seek.Right-$seek.Left)*$ratio
                Send-UiInput ('MOVE {0} {1}' -f $x.ToString([Globalization.CultureInfo]::InvariantCulture),$seek.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
                Send-UiInput 'DOWN'
                $clicked=Read-Ui
                if([Math]::Abs($clicked.MarkerRatio-$ratio) -gt .01) { throw 'Marker did not update in the acknowledged click frame.' }
                if($clicked.InputPresent -ge 0) { $inputTimes+=$clicked.InputPresent }
                Send-UiInput 'UP'
            }
            $results[-1] | Add-Member InputP50 (Percentile $inputTimes .5)
            $results[-1] | Add-Member InputP95 (Percentile $inputTimes .95)
            if($scene -ne 'compact') {
                $before=Read-Ui; Send-UiInput 'MOVE 250 300'; Send-UiInput 'WHEEL -8'
                $null=Wait-Ui {param($s) $s.ScrollY -gt $before.ScrollY} '4K list scroll responds'
            }
        }
        $modules=@($process.Modules | ForEach-Object ModuleName)
        $overlay=@($modules | Where-Object {$_ -match 'nvspcap|nvcamera|GameOverlay|RTSSHooks'})
        if($overlay.Count) { throw "Overlay modules injected: $($overlay -join ', ')" }
        Write-Host "$driver overlay module check: none"
        Capture ('benchmark-'+$driver)
    } finally {
        Remove-Item Env:PRAPPY_TEST_SOFTWARE -ErrorAction SilentlyContinue
        if(!$process.HasExited) { $null=$process.CloseMainWindow(); if(!$process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force; throw 'Benchmark did not close.' } }
    }
}
$results | Format-Table -AutoSize | Out-Host
$path=Join-Path $app 'build/ui-performance.json'; $results | ConvertTo-Json -Depth 3 | Set-Content $path -Encoding utf8
Write-Host "Timing is CPU input processing to Present return, not measured display/photon latency. Results: $path"
