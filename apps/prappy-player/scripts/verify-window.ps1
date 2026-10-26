#Requires -Version 7.0
param([switch]$Interactive, [string]$BuildVariant='Release')
$ErrorActionPreference='Stop'
$app = Split-Path $PSScriptRoot -Parent
$out = Join-Path $app 'build'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class PlayerNative {
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
 [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 [DllImport("user32.dll")] public static extern IntPtr GetWindowDpiAwarenessContext(IntPtr h);
 [DllImport("user32.dll")] public static extern bool AreDpiAwarenessContextsEqual(IntPtr a,IntPtr b);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h,ref Point p);
 [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
 [StructLayout(LayoutKind.Sequential)] public struct MinMax { public Point Reserved,MaxSize,MaxPosition,MinTrackSize,MaxTrackSize; }
 [DllImport("user32.dll",EntryPoint="SendMessageW")] public static extern IntPtr GetMinMax(IntPtr h,uint m,IntPtr w,ref MinMax info);
 [DllImport("user32.dll",EntryPoint="GetWindowRect")] static extern bool NativeGetWindowRect(IntPtr h,out Rect r);
 public static bool GetWindowRect(IntPtr h,out Rect r) { SetThreadDpiAwarenessContext(new IntPtr(-4)); return NativeGetWindowRect(h,out r); }
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern long GetWindowLongPtr(IntPtr h,int index);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll",EntryPoint="SetPhysicalCursorPos")] public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll",EntryPoint="GetPhysicalCursorPos")] public static extern bool GetCursorPos(out Point p);
 [DllImport("user32.dll")] public static extern void mouse_event(uint flags,uint x,uint y,uint data,UIntPtr extra);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
}
'@
[PlayerNative]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null
$p = Start-Process -FilePath "$app\build\$BuildVariant\PrappyPlayer.exe" -ArgumentList '--window-test' -WorkingDirectory $env:TEMP -WindowStyle Hidden -PassThru
for ($i=0; $i -lt 450; $i++) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) { break }
    if ($p.HasExited) { throw "Test player exited before creating a window: $($p.ExitCode)" }
    Start-Sleep -Milliseconds 100
}
if ($p.MainWindowHandle -eq [IntPtr]::Zero) {
    Stop-Process -Id $p.Id -Force
    throw 'Test player did not create a window.'
}
$hwnd = $p.MainWindowHandle
function Rect {
    $r = [PlayerNative+Rect]::new()
    [PlayerNative]::GetWindowRect($hwnd,[ref]$r) | Out-Null
    return $r
}
function Require($condition, $message) { if (!$condition) { throw $message }; Write-Host "PASS $message" }
function Click($x, $y) {
    [PlayerNative]::SetForegroundWindow($hwnd) | Out-Null
    $origin = [PlayerNative+Point]::new()
    [PlayerNative]::ClientToScreen($hwnd,[ref]$origin) | Out-Null
    [PlayerNative]::SetCursorPos($origin.X+$x,$origin.Y+$y) | Out-Null
    Start-Sleep -Milliseconds 80
    [PlayerNative]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [PlayerNative]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 550
}
function Capture($name) {
    $r = Rect
    $b = [Drawing.Bitmap]::new($r.Right-$r.Left,$r.Bottom-$r.Top)
    $g = [Drawing.Graphics]::FromImage($b)
    $dc = $g.GetHdc()
    try { [PlayerNative]::PrintWindow($hwnd,$dc,2) | Out-Null }
    finally { $g.ReleaseHdc($dc) }
    $b.Save((Join-Path $out "$name.png"))
    $g.Dispose(); $b.Dispose()
}
$cursor = [PlayerNative+Point]::new()
[PlayerNative]::GetCursorPos([ref]$cursor) | Out-Null
$initial = Rect
try {
    # Bring just this app forward for input; preserve its final topmost state below.
    $ex = [PlayerNative]::GetWindowLongPtr($hwnd,-20)
    [PlayerNative]::SetWindowPos($hwnd,[IntPtr](-1),120,120,500,250,0x40) | Out-Null
    [PlayerNative]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 250
    $r = Rect
    Write-Host "Test HWND $hwnd, rect $($r.Left),$($r.Top) $($r.Right-$r.Left)x$($r.Bottom-$r.Top)"
    $client = [PlayerNative+Rect]::new()
    [PlayerNative]::GetClientRect($hwnd,[ref]$client) | Out-Null
    $frameWidth = ($r.Right-$r.Left)-$client.Right
    $frameHeight = ($r.Bottom-$r.Top)-$client.Bottom
    [PlayerNative]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,(500+$frameWidth),(250+$frameHeight),0x6) | Out-Null
    $r = Rect
    $minimum = [PlayerNative+MinMax]::new()
    [PlayerNative]::GetMinMax($hwnd,0x24,[IntPtr]::Zero,[ref]$minimum) | Out-Null
    Require ($minimum.MinTrackSize.X -eq (500+$frameWidth)) '500 pixel minimum content width'
    Require ($minimum.MaxTrackSize.X -eq 0 -or ($minimum.MaxTrackSize.X-$frameWidth) -gt 900) 'No fixed width maximum'
    Require (([PlayerNative]::GetWindowLongPtr($hwnd,-16) -band 0xcf0000) -eq 0xcf0000) 'Native caption, resize frame and minimize/maximize controls'
    Require ([PlayerNative]::AreDpiAwarenessContextsEqual([PlayerNative]::GetWindowDpiAwarenessContext($hwnd),[IntPtr](-4))) 'Per-monitor v2 DPI awareness'
    Write-Host "Window DPI: $([PlayerNative]::GetDpiForWindow($hwnd))"
    Require (($ex -band 0x80) -eq 0) 'Normal taskbar/Alt-Tab window (not a tool window)'
    Require ([PlayerNative]::SendMessage($hwnd,0x7f,[IntPtr](1),[IntPtr]::Zero) -ne [IntPtr]::Zero) 'Runtime taskbar icon'
    Capture 'player-collapsed'
    [PlayerNative]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,900,520,0x6) | Out-Null
    $r = Rect
    Require (($r.Right-$r.Left) -eq 900 -and ($r.Bottom-$r.Top) -eq 520) 'Width and height resize freely'
    [PlayerNative]::SendMessage($hwnd,0x112,[IntPtr](0xf030),[IntPtr]::Zero) | Out-Null
    $r = Rect
    Require ([PlayerNative]::IsZoomed($hwnd)) 'Native maximize works'
    Require (($r.Right-$r.Left) -gt 900) 'Maximize uses screen width'
    Capture 'player-maximized'
    [PlayerNative]::SendMessage($hwnd,0x112,[IntPtr](0xf120),[IntPtr]::Zero) | Out-Null
    $r = Rect
    Require ((!([PlayerNative]::IsZoomed($hwnd))) -and ($r.Right-$r.Left) -eq 900) 'Restore preserves chosen width'
    if ($Interactive) {
    [PlayerNative]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,(500+$frameWidth),(250+$frameHeight),0x6) | Out-Null
    Start-Sleep -Milliseconds 300
    Click 465 17
    $r = Rect
    Require (($r.Bottom-$r.Top) -ge 600) 'List expands'
    Capture 'player-expanded'
    Click 465 17
    $r = Rect
    Require (($r.Bottom-$r.Top) -eq (250+$frameHeight)) 'List collapses'
    }
    [PlayerNative]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,(500+$frameWidth),(250+$frameHeight),0x6) | Out-Null
    Start-Sleep -Milliseconds 100
    $r = Rect
    $packed = [IntPtr]((($r.Top+1) -shl 16) -bor ($r.Left+250))
    Require ([PlayerNative]::SendMessage($hwnd,0x84,[IntPtr]::Zero,$packed).ToInt32() -eq 12) 'Top edge offers native height resizing'
    $packed = [IntPtr]((($r.Bottom-2) -shl 16) -bor ($r.Left+250))
    Require ([PlayerNative]::SendMessage($hwnd,0x84,[IntPtr]::Zero,$packed).ToInt32() -eq 15) 'Bottom edge offers native height resizing'
    foreach ($check in @(
        @{X=$r.Left+1;Y=$r.Top+100;Hit=10;Name='Left edge resizes horizontally'},
        @{X=$r.Right-2;Y=$r.Top+100;Hit=11;Name='Right edge resizes horizontally'},
        @{X=$r.Right-2;Y=$r.Bottom-2;Hit=17;Name='Bottom right corner offers diagonal resizing'},
        @{X=$r.Left+1;Y=$r.Top+1;Hit=13;Name='Top left corner offers diagonal resizing'}
    )) {
        $packed = [IntPtr](($check.Y -shl 16) -bor $check.X)
        Require ([PlayerNative]::SendMessage($hwnd,0x84,[IntPtr]::Zero,$packed).ToInt32() -eq $check.Hit) $check.Name
    }
    $origin = [PlayerNative+Point]::new()
    [PlayerNative]::ClientToScreen($hwnd,[ref]$origin) | Out-Null
    $packed = [IntPtr]((($origin.Y+60) -shl 16) -bor ($origin.X+30))
    Require ([PlayerNative]::SendMessage($hwnd,0x84,[IntPtr]::Zero,$packed).ToInt32() -eq 2) 'Noninteractive text offers native dragging'
    foreach ($check in @(
        @{X=250;Y=190;Hit=2;Name='Empty transport space is draggable'},
        @{X=250;Y=17;Hit=2;Name='Empty header space is draggable'},
        @{X=465;Y=17;Hit=1;Name='List button retains mouse input'},
        @{X=100;Y=150;Hit=1;Name='Play button retains mouse input'},
        @{X=250;Y=90;Hit=1;Name='Seek slider retains mouse input'},
        @{X=300;Y=150;Hit=1;Name='Volume slider retains mouse input'}
    )) {
        $packed = [IntPtr]((($origin.Y+$check.Y) -shl 16) -bor ($origin.X+$check.X))
        Require ([PlayerNative]::SendMessage($hwnd,0x84,[IntPtr]::Zero,$packed).ToInt32() -eq $check.Hit) $check.Name
    }
    $bytes = [IO.File]::ReadAllBytes($p.Path)
    $pe = [BitConverter]::ToInt32($bytes,0x3c)
    Require ([BitConverter]::ToInt16($bytes,$pe+24+68) -eq 2) 'Windows GUI executable (no console subsystem)'
    Write-Host "Screenshot: $out\player-collapsed.png. Use -Interactive for pointer-driven List checks on an idle desktop."
} finally {
    $top = if ($ex -band 8) { [IntPtr](-1) } else { [IntPtr](-2) }
    [PlayerNative]::PostMessage($hwnd,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
    if (!$p.WaitForExit(5000)) { throw 'Test player did not cancel its scan and close promptly.' }
    Require ($p.ExitCode -eq 0) 'Clean player shutdown'
    if ($Interactive) { [PlayerNative]::SetCursorPos($cursor.X,$cursor.Y) | Out-Null }
}
