#Requires -Version 7.0
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
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern long GetWindowLongPtr(IntPtr h,int index);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point p);
 [DllImport("user32.dll")] public static extern void mouse_event(uint flags,uint x,uint y,uint data,UIntPtr extra);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
}
'@
[PlayerNative]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null
$p = Start-Process -FilePath "$app\build\Release\PrappyPlayer.exe" -WorkingDirectory $env:TEMP -WindowStyle Hidden -PassThru
for ($i=0; $i -lt 50; $i++) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 100
}
if ($p.MainWindowHandle -eq [IntPtr]::Zero) { throw 'Test player did not create a window.' }
$hwnd = $p.MainWindowHandle
function Rect {
    $r = [PlayerNative+Rect]::new()
    [PlayerNative]::GetWindowRect($hwnd,[ref]$r) | Out-Null
    return $r
}
function Require($condition, $message) { if (!$condition) { throw $message }; Write-Host "PASS $message" }
function Click($x, $y) {
    $r = Rect
    [PlayerNative]::SetCursorPos($r.Left+$x,$r.Top+$y) | Out-Null
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
    [PlayerNative]::SetWindowPos($hwnd,[IntPtr](-1),120,120,500,276,0x40) | Out-Null
    [PlayerNative]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 250
    $r = Rect
    Write-Host "Test HWND $hwnd, rect $($r.Left),$($r.Top) $($r.Right-$r.Left)x$($r.Bottom-$r.Top)"
    Require (($r.Right-$r.Left) -eq 500) '500 physical pixel width'
    Require ([PlayerNative]::AreDpiAwarenessContextsEqual([PlayerNative]::GetWindowDpiAwarenessContext($hwnd),[IntPtr](-4))) 'Per-monitor v2 DPI awareness'
    Write-Host "Window DPI: $([PlayerNative]::GetDpiForWindow($hwnd))"
    Require (($ex -band 0x80) -eq 0) 'Normal taskbar/Alt-Tab window (not a tool window)'
    Require ([PlayerNative]::SendMessage($hwnd,0x7f,[IntPtr](1),[IntPtr]::Zero) -ne [IntPtr]::Zero) 'Runtime taskbar icon'
    Capture 'player-collapsed'
    [PlayerNative]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,900,520,0x6) | Out-Null
    $r = Rect
    Require (($r.Right-$r.Left) -eq 500 -and ($r.Bottom-$r.Top) -eq 520) 'Width clamped during native height resize'
    [PlayerNative]::SendMessage($hwnd,0x112,[IntPtr](0xf030),[IntPtr]::Zero) | Out-Null
    $r = Rect
    Require (($r.Right-$r.Left) -eq 500) 'Maximize cannot expand width'
    Click 365 20
    $r = Rect
    Require (($r.Bottom-$r.Top) -ge 600) 'List expands'
    Capture 'player-expanded'
    Click 365 20
    $r = Rect
    Require (($r.Bottom-$r.Top) -eq 276) 'List collapses'
    $originalTop = if ($ex -band 8) { [IntPtr](-1) } else { [IntPtr](-2) }
    [PlayerNative]::SetWindowPos($hwnd,$originalTop,0,0,0,0,0x13) | Out-Null
    $before = [PlayerNative]::GetWindowLongPtr($hwnd,-20)
    Click 417 17
    $after = [PlayerNative]::GetWindowLongPtr($hwnd,-20)
    Require (($before -band 8) -ne ($after -band 8)) 'Pin toggles always-on-top'
    Click 417 17
    $r = Rect
    $packed = [IntPtr]((($r.Top+1) -shl 16) -bor ($r.Left+250))
    Require ([PlayerNative]::SendMessage($hwnd,0x84,[IntPtr]::Zero,$packed).ToInt32() -eq 12) 'Top edge offers native height resizing'
    $packed = [IntPtr]((($r.Bottom-2) -shl 16) -bor ($r.Left+250))
    Require ([PlayerNative]::SendMessage($hwnd,0x84,[IntPtr]::Zero,$packed).ToInt32() -eq 15) 'Bottom edge offers native height resizing'
    $packed = [IntPtr]((($r.Top+60) -shl 16) -bor ($r.Left+30))
    Require ([PlayerNative]::SendMessage($hwnd,0x84,[IntPtr]::Zero,$packed).ToInt32() -eq 2) 'Angled transport strip offers native dragging'
    $bytes = [IO.File]::ReadAllBytes($p.Path)
    $pe = [BitConverter]::ToInt32($bytes,0x3c)
    Require ([BitConverter]::ToInt16($bytes,$pe+24+68) -eq 2) 'Windows GUI executable (no console subsystem)'
    Write-Host "Screenshots: $out\player-{collapsed,expanded}.png"
} finally {
    $top = if ($ex -band 8) { [IntPtr](-1) } else { [IntPtr](-2) }
    [PlayerNative]::PostMessage($hwnd,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
    [PlayerNative]::SetCursorPos($cursor.X,$cursor.Y) | Out-Null
}
