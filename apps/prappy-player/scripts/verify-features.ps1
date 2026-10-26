#Requires -Version 7.0
param([string]$BuildVariant='Release')
$ErrorActionPreference='Stop'
$app = Split-Path $PSScriptRoot -Parent
$fixture = Join-Path $app 'build/dedup-test/scraper/library'
$stateDirectory = Join-Path $app ("build/feature-ui/$BuildVariant-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$stateDirectory/media" | Out-Null
Copy-Item -LiteralPath "$fixture/a/same.wav" -Destination "$stateDirectory/media/alpha.wav"
Copy-Item -LiteralPath "$fixture/b/same.wav" -Destination "$stateDirectory/media/beta.wav"
@("directory=$stateDirectory/media",'extensions=wav','history_limit=10000') | Set-Content "$stateDirectory/player.ini" -Encoding utf8
$alpha = ('"{0}"' -f ("$stateDirectory/media/alpha.wav".Replace('\','\\')))
$beta = ('"{0}"' -f ("$stateDirectory/media/beta.wav".Replace('\','\\')))
@('PRAPPY_HISTORY 1','NEXT_ID 3','REPEAT 0',"ENTRY 1 1700000000 0 $alpha", "ENTRY 2 1700000010 1 $beta",'TRAIL 1 2','CURSOR 2') | Set-Content "$stateDirectory/player-history.txt" -Encoding utf8NoBOM
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class FeatureNative {
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
'@
[FeatureNative]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null
$process = Start-Process -FilePath "$app/build/$BuildVariant/PrappyPlayer.exe" -WindowStyle Hidden -PassThru `
    -ArgumentList '--window-test',('"{0}"' -f $stateDirectory)
$hwnd = [IntPtr]::Zero
$script:inputSequence = 0
function Require($condition,[string]$message) { if (!$condition) { throw $message }; Write-Host "PASS $message" }
function Read-Shared([string]$path) {
    $stream = [IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function StateText { Read-Shared "$stateDirectory/player-history.txt" }
function Read-Ui {
    if (!(Test-Path -LiteralPath "$stateDirectory/ui-status.txt")) { return $null }
    try { $text = Read-Shared "$stateDirectory/ui-status.txt" }
    catch [IO.IOException] { return $null } # Replacement may overlap this read; poll for the next complete snapshot.
    $lines = $text -split '\r?\n'; $ready = $lines[0] -split ' '
    if ($ready[0] -ne 'READY') { return $null }
    $state = [pscustomobject]@{Width=[int]$ready[1];Height=[int]$ready[2];Repeat=[int]$ready[3];Cursor=[uint64]$ready[4];Count=[int]$ready[5];Pending=[bool][int]$ready[6];Paused=[bool][int]$ready[7];History=[bool][int]$ready[8];Visible=[int]$ready[9];Ack=[uint64]$ready[10];Controls=@{};Tooltip=''}
    foreach ($line in $lines) {
        if ($line -match '^CONTROL "([^"]+)" ([^ ]+) ([^ ]+) ([^ ]+) ([^ ]+) ([01]) ([01]) ([0-9]+)$') {
            $state.Controls[$matches[1]] = [pscustomobject]@{
                X=([double]::Parse($matches[2],[Globalization.CultureInfo]::InvariantCulture)+[double]::Parse($matches[4],[Globalization.CultureInfo]::InvariantCulture))/2
                Y=([double]::Parse($matches[3],[Globalization.CultureInfo]::InvariantCulture)+[double]::Parse($matches[5],[Globalization.CultureInfo]::InvariantCulture))/2
                Hovered=[bool][int]$matches[6];Active=[bool][int]$matches[7];Clicks=[uint64]$matches[8]
            }
        } elseif ($line -match '^TOOLTIP "(.*)"$') { $state.Tooltip=$matches[1] }
    }
    return $state
}
function Wait-Ui([scriptblock]$condition,[string]$description) {
    $timer=[Diagnostics.Stopwatch]::StartNew(); $state=$null
    while ($timer.Elapsed.TotalSeconds -lt 10) {
        $state=Read-Ui
        if ($null -ne $state -and (& $condition $state)) { return $state }
        if ($process.HasExited) { throw 'Feature test player exited unexpectedly.' }
        # Poll an observable condition; no fixed delay is used to assume readiness.
        Start-Sleep -Milliseconds 10
    }
    Capture 'failure'
    throw "UI condition timed out: $description. State: $($state | ConvertTo-Json -Compress -Depth 3)"
}
function Wait-State([string]$pattern) {
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt 10) {
        try { if ((StateText) -match $pattern) { return } } catch [IO.IOException] { }
        if ($process.HasExited) { throw 'Feature test player exited unexpectedly.' }
        Start-Sleep -Milliseconds 10
    }
    throw "Saved state did not match $pattern"
}
function Send-UiInput([string]$action) {
    $script:inputSequence++
    [IO.File]::WriteAllText("$stateDirectory/ui-command.txt","$script:inputSequence $action`n",[Text.UTF8Encoding]::new($false))
    $null=Wait-Ui {param($s) $s.Ack -eq $script:inputSequence} "input $script:inputSequence acknowledged"
}
function Click([string]$name) {
    $state = Wait-Ui {param($s) $s.Controls.ContainsKey($name)} "control $name rendered"
    $control=$state.Controls[$name]; $before=$control.Clicks
    $x=$control.X.ToString([Globalization.CultureInfo]::InvariantCulture)
    $y=$control.Y.ToString([Globalization.CultureInfo]::InvariantCulture)
    Send-UiInput "MOVE $x $y"
    $null=Wait-Ui {param($s) $s.Controls.ContainsKey($name) -and $s.Controls[$name].Hovered} "control $name hovered"
    Send-UiInput 'DOWN'
    try { $null=Wait-Ui {param($s) $s.Controls.ContainsKey($name) -and $s.Controls[$name].Active} "control $name pressed" }
    finally { Send-UiInput 'UP' }
    $null=Wait-Ui {param($s) $s.Controls.ContainsKey($name) -and $s.Controls[$name].Clicks -gt $before} "control $name activated"
}
function Capture([string]$name) {
    $rect = [FeatureNative+Rect]::new()
    [FeatureNative]::GetWindowRect($hwnd,[ref]$rect) | Out-Null
    $bitmap = [Drawing.Bitmap]::new($rect.Right-$rect.Left,$rect.Bottom-$rect.Top)
    $graphics = [Drawing.Graphics]::FromImage($bitmap); $dc=$graphics.GetHdc()
    try { [FeatureNative]::PrintWindow($hwnd,$dc,2) | Out-Null }
    finally { $graphics.ReleaseHdc($dc) }
    try { $bitmap.Save("$stateDirectory/$name.png") } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
try {
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt 10) {
        $process.Refresh(); $hwnd=$process.MainWindowHandle
        if ($hwnd -ne [IntPtr]::Zero) { break }
        if ($process.HasExited) { throw 'Feature test player exited during startup.' }
        Start-Sleep -Milliseconds 10
    }
    Require ($hwnd -ne [IntPtr]::Zero) 'Feature test window opened'
    [FeatureNative]::SetWindowPos($hwnd,[IntPtr]::Zero,120,120,0,0,0x45) | Out-Null
    $null=Wait-Ui {param($s) $s.Count -eq 2 -and !$s.Pending -and $s.Paused} 'session restored paused'
    Capture 'startup'
    Click 'history'
    $null=Wait-Ui {param($s) $s.History -and $s.Height -ge 600 -and $s.Visible -eq 2} 'History expanded with restored visits'
    Write-Host 'PASS History opens directly from compact controls'
    Capture 'history-restored'
    foreach ($mode in @(1,2,0)) {
        Click 'repeat'
        Wait-State "(?m)^REPEAT $mode\r?$"
        $expected=@('Repeat: Off. Click to repeat one track.','Repeat: One. Click to repeat all tracks.','Repeat: All. Click to turn repeat off.')[$mode]
        $null=Wait-Ui {param($s) $s.Tooltip -eq $expected} "repeat tooltip for mode $mode"
        Capture "repeat-$mode"
        Write-Host "PASS Repeat button persists state $mode"
    }
    Click 'back'
    Capture 'back-click'
    Wait-State '(?m)^CURSOR 1\r?$'
    $null=Wait-Ui {param($s) !$s.Pending -and $s.Cursor -eq 1} 'Back load committed'
    Click 'play'
    $null=Wait-Ui {param($s) $s.Paused} 'paused after Back'
    Click 'next'
    Wait-State '(?m)^CURSOR 2\r?$'
    $null=Wait-Ui {param($s) !$s.Pending -and $s.Cursor -eq 2} 'Forward load committed'
    Click 'play'
    $null=Wait-Ui {param($s) $s.Paused} 'paused after Forward'
    Require (([regex]::Matches((StateText),'(?m)^ENTRY ')).Count -eq 2) 'Back/Forward do not append visits'
    Click 'history-1'
    Wait-State '(?m)^NEXT_ID 4\r?$'
    Wait-State '(?m)^CURSOR 3\r?$'
    $null=Wait-Ui {param($s) !$s.Pending -and $s.Cursor -eq 3} 'history selection loaded'
    Click 'play'
    $null=Wait-Ui {param($s) $s.Paused} 'paused after selection'
    Require (([regex]::Matches((StateText),'(?m)^ENTRY ')).Count -eq 3) 'Selecting a history row adds a fresh visit'
    Click 'back'
    Wait-State '(?m)^CURSOR 2\r?$'
    $null=Wait-Ui {param($s) !$s.Pending -and $s.Cursor -eq 2} 'Back returns to prior selected visit'
    Click 'play'
    $null=Wait-Ui {param($s) $s.Paused} 'paused before search'
    Click 'search'
    Send-UiInput 'TEXT "alpha"'
    $null=Wait-Ui {param($s) $s.Visible -eq 2} 'history search filters three visits to two matches'
    Capture 'history-filtered'
    Write-Host "Feature screenshots: $stateDirectory"
} finally {
    if (!$process.HasExited) {
        if ($hwnd -ne [IntPtr]::Zero) { [FeatureNative]::PostMessage($hwnd,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
        if (!$process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force; throw 'Feature test player did not close promptly.' }
    }
}
Require ($process.ExitCode -eq 0) 'Feature test clean shutdown'
