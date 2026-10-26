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
 [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h,ref Point p);
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
                Left=[double]::Parse($matches[2],[Globalization.CultureInfo]::InvariantCulture)
                Top=[double]::Parse($matches[3],[Globalization.CultureInfo]::InvariantCulture)
                Right=[double]::Parse($matches[4],[Globalization.CultureInfo]::InvariantCulture)
                Bottom=[double]::Parse($matches[5],[Globalization.CultureInfo]::InvariantCulture)
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
function Check-HeaderText([string]$name,$layout) {
    $window=[FeatureNative+Rect]::new(); $origin=[FeatureNative+Point]::new()
    [FeatureNative]::GetWindowRect($hwnd,[ref]$window) | Out-Null
    [FeatureNative]::ClientToScreen($hwnd,[ref]$origin) | Out-Null
    $bitmap=[Drawing.Bitmap]::new("$stateDirectory/$name.png")
    try {
        $bounds=@{}
        foreach ($controlName in @('history','list')) {
            $control=$layout.Controls[$controlName]
            $left=[int]$control.Left+$origin.X-$window.Left
            $right=[int]$control.Right+$origin.X-$window.Left
            $top=[int]$control.Top+$origin.Y-$window.Top
            $bottom=[int]$control.Bottom+$origin.Y-$window.Top
            $firstInk=$right
            for ($x=$left+3; $x -lt $right-3; $x++) {
                for ($y=$top+2; $y -lt $bottom-2; $y++) {
                    $pixel=$bitmap.GetPixel($x,$y)
                    if ($pixel.R -gt 140 -and $pixel.G -gt 140 -and $pixel.B -gt 140) { $firstInk=$x; break }
                }
                if ($firstInk -ne $right) { break }
            }
            Require ($firstInk -lt $right) "$controlName label is visible in screenshot"
            $inkTop=$bottom; $inkBottom=$top
            # Compare the initial capitals H and L, excluding History's y descender.
            for ($x=$firstInk; $x -lt $firstInk+3; $x++) {
                for ($y=$top+2; $y -lt $bottom-2; $y++) {
                    $pixel=$bitmap.GetPixel($x,$y)
                    if ($pixel.R -gt 140 -and $pixel.G -gt 140 -and $pixel.B -gt 140) {
                        $inkTop=[Math]::Min($inkTop,$y); $inkBottom=[Math]::Max($inkBottom,$y)
                    }
                }
            }
            $bounds[$controlName]=@($inkTop,$inkBottom)
        }
        Require ($bounds.history[0] -eq $bounds.list[0] -and $bounds.history[1] -eq $bounds.list[1]) "History/List rendered capitals share top and baseline ($name): H=$($bounds.history -join ',') L=$($bounds.list -join ',')"
    } finally { $bitmap.Dispose() }
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
    $layout=Read-Ui
    Check-HeaderText 'startup' $layout
    Require ($layout.Controls.ContainsKey('volume-value') -and $layout.Controls.ContainsKey('volume')) 'Volume value is outside the interactive slider'
    Require ($layout.Controls['volume-value'].Right -lt $layout.Controls['volume'].Left) 'Volume value cannot be covered by the slider thumb'
    $header=$layout.Controls['history']; $list=$layout.Controls['list']
    Require ($header.Top -eq $list.Top -and $header.Bottom -eq $list.Bottom) 'History and List have identical vertical bounds'
    Require (($header.Right-$header.Left) -eq ($list.Right-$list.Left)) 'History and List have identical widths'
    $transport=$layout.Controls['play']
    foreach ($name in @('back','next','repeat','refresh')) {
        Require ($layout.Controls[$name].Top -eq $transport.Top -and $layout.Controls[$name].Bottom -eq $transport.Bottom) "Transport $name has matching vertical bounds"
    }
    foreach ($endpoint in @(0,100)) {
        $slider=(Read-Ui).Controls['volume']
        $x=if ($endpoint -eq 0) {$slider.Left+1} else {$slider.Right-1}
        Send-UiInput ('MOVE {0} {1}' -f $x.ToString([Globalization.CultureInfo]::InvariantCulture),$slider.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
        $null=Wait-Ui {param($s) $s.Controls['volume'].Hovered} 'volume slider hovered'
        Send-UiInput 'DOWN'
        $null=Wait-Ui {param($s) $s.Controls['volume'].Active} 'volume slider pressed'
        Send-UiInput 'UP'
        $null=Wait-Ui {param($s) $s.Tooltip -eq "Volume: $endpoint%"} "volume endpoint $endpoint applied"
        $endpointLayout=Read-Ui
        Require ($endpointLayout.Controls['volume-value'].Right -lt $endpointLayout.Controls['volume'].Left) "Volume $endpoint readout stays outside the slider"
        Capture "volume-$endpoint"
    }
    $rect=[FeatureNative+Rect]::new()
    [FeatureNative]::GetWindowRect($hwnd,[ref]$rect) | Out-Null
    $borderWidth=($rect.Right-$rect.Left)-$layout.Width; $borderHeight=($rect.Bottom-$rect.Top)-$layout.Height
    [FeatureNative]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,(900+$borderWidth),(250+$borderHeight),0x6) | Out-Null
    $wide=Wait-Ui {param($s) $s.Width -eq 900} 'wide layout rendered'
    Require ($wide.Controls['history'].Top -eq $wide.Controls['list'].Top -and $wide.Controls['history'].Bottom -eq $wide.Controls['list'].Bottom) 'Wide header controls remain aligned'
    Require ($wide.Controls['volume'].Right -lt $wide.Controls['repeat'].Left -and $wide.Controls['refresh'].Right -le 900) 'Wide transport remains spaced within the window'
    Capture 'wide-layout'
    Check-HeaderText 'wide-layout' $wide
    [FeatureNative]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,(500+$borderWidth),(250+$borderHeight),0x6) | Out-Null
    $null=Wait-Ui {param($s) $s.Width -eq 500} 'compact width restored'
    Click 'history'
    $null=Wait-Ui {param($s) $s.History -and $s.Height -ge 600 -and $s.Visible -eq 2} 'History expanded with restored visits'
    Write-Host 'PASS History opens directly from compact controls'
    Capture 'history-restored'
    Check-HeaderText 'history-restored' (Read-Ui)
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
