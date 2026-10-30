param([string]$Variant='Release',[string]$Label='before',[int]$Seconds=8,[switch]$NativeWait)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$buildRoot=Join-Path $app 'build'
$state=Join-Path $buildRoot ('idle-'+$Label+'-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory "$state/media" -Force | Out-Null
Copy-Item "$buildRoot/dedup-test/scraper/library/a/same.wav" "$state/media/a.wav"
Copy-Item "$buildRoot/dedup-test/scraper/library/b/same.wav" "$state/media/b.wav"
@("directory=$state/media",'extensions=wav') | Set-Content "$state/player.ini" -Encoding utf8
$media=('"{0}"' -f "$state/media/a.wav".Replace('\','\\'))
@('PRAPPY_HISTORY 1','NEXT_ID 2','REPEAT 2',"ENTRY 1 1700000000 0.5 $media",'TRAIL 1','CURSOR 1') | Set-Content "$state/player-history.txt" -Encoding utf8NoBOM
Add-Type @'
using System;using System.Runtime.InteropServices;
public static class IdleNative {
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int w,int height,uint f);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
'@
$process=Start-Process "$app/build/$Variant/PrappyPlayer.exe" -WindowStyle Hidden -PassThru -ArgumentList '--window-test',('"{0}"' -f $state)
$records=@()
try {
    $timer=[Diagnostics.Stopwatch]::StartNew()
    do {
        $process.Refresh()
        $ready=Test-Path "$state/ui-status.txt"
        $caches=@(Get-ChildItem "$state/player-analysis-v2/wave-*.bin" -ErrorAction SilentlyContinue).Count
        if($ready -and $caches -eq 2 -and $process.MainWindowHandle -ne [IntPtr]::Zero){break}
        if($timer.Elapsed.TotalSeconds -gt 20 -or $process.HasExited){throw 'Benchmark startup failed.'}
        Start-Sleep -Milliseconds 25
    }while($true)
    if ($NativeWait) { [IO.File]::WriteAllText("$state/ui-command.txt", "1 IDLE`n"); Start-Sleep -Seconds 2 }
    $hwnd=$process.MainWindowHandle
    [IdleNative]::SetWindowPos($hwnd,[IntPtr]::Zero,100,100,0,0,0x15) | Out-Null
    foreach($mode in @('paused-compact','paused-expanded','minimized','playing')) {
        if($mode -eq 'paused-expanded'){[IdleNative]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,900,760,0x16)|Out-Null}
        if($mode -eq 'minimized'){[IdleNative]::ShowWindow($hwnd,6)|Out-Null}
        if($mode -eq 'playing'){
            [IdleNative]::ShowWindow($hwnd,9)|Out-Null
            [IdleNative]::PostMessage($hwnd,0x100,[IntPtr]32,[IntPtr]0)|Out-Null
            [IdleNative]::PostMessage($hwnd,0x101,[IntPtr]32,[IntPtr]0)|Out-Null
        }
        Start-Sleep -Milliseconds 1000
        $process.Refresh();$cpu=$process.TotalProcessorTime.TotalSeconds
        $elapsed=[Diagnostics.Stopwatch]::StartNew()
        Start-Sleep -Seconds $Seconds
        $process.Refresh(); if ($process.HasExited) { throw "Benchmark player exited unexpectedly." }; $used=$process.TotalProcessorTime.TotalSeconds-$cpu
        $record=[pscustomobject]@{label=$Label;mode=$mode;wall_seconds=$elapsed.Elapsed.TotalSeconds;cpu_seconds=$used;core_percent=100*$used/$elapsed.Elapsed.TotalSeconds;working_set_mb=$process.WorkingSet64/1MB;state=$state}
        $records+=$record;$record|ConvertTo-Json -Compress|Write-Output
    }
} finally {
    if(!$process.HasExited){$null=$process.CloseMainWindow();if(!$process.WaitForExit(5000)){Stop-Process -Id $process.Id -Force}}
}
$records | ConvertTo-Json | Set-Content "$buildRoot/idle-$Label.json"
