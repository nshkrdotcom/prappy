#Requires -Version 7.0
# Shared UI probes and input helpers; callers own their process and state root.
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
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int command);
 [DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h,int index);
}
'@
[FeatureNative]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null
function Require($condition,[string]$message) { if (!$condition) { throw $message }; Write-Host "PASS $message" }
function Read-Shared([string]$path) {
    $stream = [IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function StateText { Read-Shared "$stateDirectory/player-history.txt" }
function Read-Ui {
    if ($script:cachedUiProcess -ne $process.Id) { $script:cachedUi=$null; $script:cachedUiProcess=$process.Id }
    $text=$null
    for ($readAttempt=0; $readAttempt -lt 50; ++$readAttempt) {
        try {
            if (Test-Path -LiteralPath "$stateDirectory/ui-status.txt") { $text = Read-Shared "$stateDirectory/ui-status.txt"; break }
        }
        catch [IO.IOException] {
            # Replacement may temporarily remove the name or overlap opening a reader.
        }
        Start-Sleep -Milliseconds 5
    }
    if (!$text) { return $script:cachedUi }
    $lines = $text -split '\r?\n'; $ready = $lines[0] -split ' '
    if ($ready[0] -ne 'READY') { return $null }
    $state = [pscustomobject]@{Width=[int]$ready[1];Height=[int]$ready[2];Repeat=[int]$ready[3];Cursor=[uint64]$ready[4];Count=[int]$ready[5];Pending=[bool][int]$ready[6];Paused=[bool][int]$ready[7];History=[bool][int]$ready[8];Visible=[int]$ready[9];Ack=[uint64]$ready[10];Controls=@{};Activations=@{};Tooltip='';ScrollY=0;ScrollMaxY=0;SortOrder=0;Topmost=$false;Strip=$false;Normalize=$false;Gain=0;Position=0;Duration=0;Waveform=0;Queued=0;View=0;Scoped=$false;Peak=0;Sample=$null;Catalog=$null}
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
        elseif ($line -match '^SCROLL ([^ ]+) ([^ ]+) ([^ ]+)$') {
            $state.ScrollY=[double]::Parse($matches[2],[Globalization.CultureInfo]::InvariantCulture)
            $state.ScrollMaxY=[double]::Parse($matches[3],[Globalization.CultureInfo]::InvariantCulture)
        }
        elseif ($line.StartsWith('SAMPLER ')) { $state.Sample=$line.Substring(8)|ConvertFrom-Json }
        elseif ($line.StartsWith('CATALOG ')) { $state.Catalog=$line.Substring(8)|ConvertFrom-Json }
        elseif ($line -match '^ORDER ([0-9]+)$') { $state.SortOrder=[int]$matches[1] }
        elseif ($line -match '^SESSION (.+)$') { $state | Add-Member Session $matches[1] -Force }
        elseif ($line -match '^RENDERER (.+)$') { $state | Add-Member Renderer $matches[1] -Force }
        elseif ($line -match '^PLAYBACK_ERROR(.*)$') { $state | Add-Member PlaybackError $matches[1].Trim() -Force }
        elseif ($line -match '^FRAME (\S+) (\S+) (\S+) (\S+) (\S+)$') {
            $state | Add-Member FrameBuild ([double]::Parse($matches[1],[Globalization.CultureInfo]::InvariantCulture)) -Force
            $state | Add-Member FrameDraw ([double]::Parse($matches[2],[Globalization.CultureInfo]::InvariantCulture)) -Force
            $state | Add-Member FramePresent ([double]::Parse($matches[3],[Globalization.CultureInfo]::InvariantCulture)) -Force
            $state | Add-Member FrameTotal ([double]::Parse($matches[4],[Globalization.CultureInfo]::InvariantCulture)) -Force
            $state | Add-Member InputPresent ([double]::Parse($matches[5],[Globalization.CultureInfo]::InvariantCulture)) -Force
        }
        elseif ($line -match '^TEXT ([0-9]+) ([0-9]+) ([0-9]+)$') {
            $state | Add-Member TextShaped ([uint64]$matches[1]) -Force; $state | Add-Member TextRasterized ([uint64]$matches[2]) -Force; $state | Add-Member TextBytes ([uint64]$matches[3]) -Force
        }
        elseif ($line -match '^DRAWN ([0-9]+)$') { $state | Add-Member DrawnTracks ([int]$matches[1]) -Force }
        elseif ($line -match '^PANEL ([0-9]+) ([0-9]+)$') { $state | Add-Member PanelPreferred ([int]$matches[1]) -Force; $state | Add-Member PanelHeight ([int]$matches[2]) -Force }
        elseif ($line -match '^WAVE ([0-9]+) ([0-9]+) ([^ ]+) ([0-9]+)$') {
            $state | Add-Member WaveBuilds ([int]$matches[1]) -Force; $state | Add-Member WaveVertices ([int]$matches[2]) -Force
            $state | Add-Member MarkerRatio ([double]::Parse($matches[3],[Globalization.CultureInfo]::InvariantCulture)) -Force
            $state | Add-Member SeekSequence ([uint64]$matches[4]) -Force
        }
        elseif ($line -match '^DIRS ([0-9]+) ([0-9]+) ([0-9]+)$') {
            $state | Add-Member Included ([int]$matches[1]) -Force; $state | Add-Member Hidden ([int]$matches[2]) -Force; $state | Add-Member Pins ([int]$matches[3]) -Force
        }
        elseif ($line -match '^LOOP ([0-9]+) ([0-9]+) ([01]) ([01])$') {
            $state | Add-Member Presents ([uint64]$matches[1]) -Force
            $state | Add-Member TextInput ([bool][int]$matches[3]) -Force
            $state | Add-Member Composing ([bool][int]$matches[4]) -Force
        }
        elseif ($line -match '^ACTIVATION "([^"]+)" ([0-9]+)$') { $state.Activations[$matches[1]]=[uint64]$matches[2] }
        elseif ($line.StartsWith('FEATURES ')) {
            $feature=$line -split ' '
            $state.Topmost=[bool][int]$feature[1]; $state.Strip=[bool][int]$feature[2]; $state.Normalize=[bool][int]$feature[3]
            $state.Gain=[double]::Parse($feature[4],[Globalization.CultureInfo]::InvariantCulture)
            $state.Position=[double]::Parse($feature[5],[Globalization.CultureInfo]::InvariantCulture)
            $state.Duration=[double]::Parse($feature[6],[Globalization.CultureInfo]::InvariantCulture)
            $state.Waveform=[uint64]$feature[7]; $state.Queued=[int]$feature[8]; $state.View=[int]$feature[9]; $state.Scoped=[bool][int]$feature[10]
            $state.Peak=[double]::Parse($feature[11],[Globalization.CultureInfo]::InvariantCulture)
            if ([int]$feature[12] -gt 0) { throw 'Dear ImGui reported a layout/input error.' }
        }
    }
    $script:cachedUi=$state
    return $state
}
function Wait-Ui([scriptblock]$condition,[string]$description,[int]$Timeout=10) {
    $timer=[Diagnostics.Stopwatch]::StartNew(); $state=$null
    while ($timer.Elapsed.TotalSeconds -lt $Timeout) {
        if ($hwnd -ne [IntPtr]::Zero -and [FeatureNative]::IsIconic($hwnd)) { [FeatureNative]::ShowWindow($hwnd,9) | Out-Null }
        $state=Read-Ui
        if ($null -ne $state -and (& $condition $state)) { return $state }
        if ($process.HasExited) { throw 'Feature test player exited unexpectedly.' }
        # Poll an observable condition; no fixed delay is used to assume readiness.
        Start-Sleep -Milliseconds 10
    }
    try { Capture 'failure' } catch { Write-Warning ('Failure screenshot unavailable: '+$_.Exception.Message) }
    throw "UI condition timed out: $description. Probe: $stateDirectory/ui-status.txt. Playback: $($state.PlaybackError) Editor: $($state.Sample.notice)"
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
function Move-Control([string]$name) {
    for ($attempt=0; $attempt -lt 5; $attempt++) {
        $state=Wait-Ui {param($s) $s.Controls.ContainsKey($name)} "control $name rendered"
        $control=$state.Controls[$name]
        $x=$control.X.ToString([Globalization.CultureInfo]::InvariantCulture)
        $y=$control.Y.ToString([Globalization.CultureInfo]::InvariantCulture)
        Send-UiInput "MOVE $x $y"
        $hover=Wait-Ui {param($s) $s.Controls.ContainsKey($name) -and ($s.Controls[$name].Hovered -or [Math]::Abs($s.Controls[$name].X-$control.X) -gt 1 -or [Math]::Abs($s.Controls[$name].Y-$control.Y) -gt 1)} "control $name hovered or repositioned"
        if ($hover.Controls[$name].Hovered) { return $hover.Controls[$name] }
    }
    throw "Control $name did not settle in its popup."
}
function Press-Control([string]$name) {
    $before=(Read-Ui).Controls[$name].Clicks
    Send-UiInput 'DOWN'
    try { $null=Wait-Ui {param($s) $s.Controls.ContainsKey($name) -and $s.Controls[$name].Active} "control $name pressed" }
    finally { Send-UiInput 'UP' }
    $null=Wait-Ui {param($s) $s.Activations.ContainsKey($name) -and $s.Activations[$name] -gt $before} "control $name activated"
}
function Click([string]$name) { $null=Move-Control $name; Press-Control $name }
function Click-At([string]$name,[double]$X,[double]$Y) {
    Send-UiInput ('MOVE {0} {1}' -f $X.ToString([Globalization.CultureInfo]::InvariantCulture),$Y.ToString([Globalization.CultureInfo]::InvariantCulture))
    $null=Wait-Ui {param($s) $s.Controls.ContainsKey($name) -and $s.Controls[$name].Hovered} "control $name hovered"
    Press-Control $name
}
function Dismiss-Popup {
    for($attempt=0;$attempt -lt 3;$attempt++) {
        Send-UiInput 'KEY ESC 1'; Send-UiInput 'KEY ESC 0'
        if (!(Read-Ui).Controls.ContainsKey('directory-all')) { break } # Escape first leaves an active text field.
    }
}
function Context([string]$name) {
    $null=Move-Control $name
    Send-UiInput 'RIGHT_DOWN'; Send-UiInput 'RIGHT_UP'
    $null=Wait-Ui {param($s) $s.Controls.ContainsKey('queue-next')} 'track context menu opened'
}
function Select-Combo([string]$name,[int]$index) {
    Click $name; Send-UiInput 'TAP HOME'
    for ($i=0; $i -lt $index; $i++) { Send-UiInput 'TAP DOWN' }
    Send-UiInput 'TAP ENTER'
}
function Search([string]$query) {
    Click 'search'
    Send-UiInput 'KEY CTRL 1'; Send-UiInput 'KEY A 1'; Send-UiInput 'KEY A 0'; Send-UiInput 'KEY CTRL 0'
    Send-UiInput ('TEXT "{0}"' -f $query)
}
function Check-DirectoryRow([string]$name) {
    $state=Wait-Ui {param($s) $s.Controls.ContainsKey('directories') -and $s.Controls.ContainsKey('directory-overflow')} "$name directory row rendered"
    $controls=@($state.Controls.GetEnumerator() | Where-Object { $_.Key -eq 'directories' -or $_.Key -eq 'directory-reset' -or $_.Key -eq 'directory-overflow' -or $_.Key -like 'directory-chip-*' } | ForEach-Object Value | Sort-Object Left)
    $right=-1
    foreach($control in $controls) {
        Require ($control.Left -ge $right -and $control.Right -le $state.Width-8 -and [Math]::Abs($control.Y-$state.Controls['directories'].Y) -lt 1) "$name directory control stays readable within its row"
        $right=$control.Right
    }
    Require ($state.Controls['directories'].Left -lt $state.Controls['directory-overflow'].Left) "$name retains Folders before overflow"
}
function Capture([string]$name) {
    $layout=Read-Ui
    if (!$layout.Strip -and $layout.Controls.ContainsKey('footer')) {
        $footer=$layout.Controls['footer']; $play=$layout.Controls['play']
        Require ($footer.Top -lt $layout.Controls['seek'].Top) "$name status precedes waveform"
        Require ([Math]::Abs(($layout.Height-$play.Bottom)-$layout.Controls['back'].Left) -le 2) "$name playback row has equal bottom and left margins"
        Require ($layout.Controls['seek'].Bottom -le $layout.Controls['transport-status'].Top -and
            $layout.Controls['transport-status'].Bottom -lt $play.Top) "$name waveform, status and transport do not overlap"
    }
    $rect = [FeatureNative+Rect]::new()
    [FeatureNative]::GetWindowRect($hwnd,[ref]$rect) | Out-Null
    $bitmap = [Drawing.Bitmap]::new($rect.Right-$rect.Left,$rect.Bottom-$rect.Top)
    $graphics = [Drawing.Graphics]::FromImage($bitmap); $dc=$graphics.GetHdc()
    try { [FeatureNative]::PrintWindow($hwnd,$dc,2) | Out-Null }
    finally { $graphics.ReleaseHdc($dc) }
    try { $bitmap.Save("$stateDirectory/$name.png") } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
function Check-HeaderText([string]$name,$layout) {
    if ($layout.Controls.ContainsKey('transport-status')) {
        $label=$layout.Controls['transport-status']; $meter=$layout.Controls['peak-meter']
        Require ([Math]::Abs($label.Y-$meter.Y) -le 0.5 -and [Math]::Abs($label.Top-$meter.Top) -le 0.5 -and
            [Math]::Abs($label.Bottom-$meter.Bottom) -le 0.5) "Playback status and meter share a centered row ($name)"
    }
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
