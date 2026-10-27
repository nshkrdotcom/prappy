#Requires -Version 7.0
param([string]$BuildVariant='Release')
$ErrorActionPreference='Stop'
$app = Split-Path $PSScriptRoot -Parent
$fixture = Join-Path $app 'build/dedup-test/scraper/library'
$stateDirectory = Join-Path $app ("build/feature-ui/$BuildVariant-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$stateDirectory/media" | Out-Null
Copy-Item -LiteralPath "$fixture/a/same.wav" -Destination "$stateDirectory/media/alpha.wav"
Copy-Item -LiteralPath "$fixture/b/same.wav" -Destination "$stateDirectory/media/beta.wav"
# Force the restored beta path to be a hidden duplicate, exercising canonical analysis identity.
Copy-Item -LiteralPath "$fixture/b/same.wav" -Destination "$stateDirectory/media/a-beta-copy.wav"
@("directory=$stateDirectory/media",'extensions=wav,au','history_limit=10000') | Set-Content "$stateDirectory/player.ini" -Encoding utf8
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
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int command);
 [DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h,int index);
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
    $state = [pscustomobject]@{Width=[int]$ready[1];Height=[int]$ready[2];Repeat=[int]$ready[3];Cursor=[uint64]$ready[4];Count=[int]$ready[5];Pending=[bool][int]$ready[6];Paused=[bool][int]$ready[7];History=[bool][int]$ready[8];Visible=[int]$ready[9];Ack=[uint64]$ready[10];Controls=@{};Activations=@{};Tooltip='';ScrollY=0;ScrollMaxY=0;SortOrder=0;Topmost=$false;Strip=$false;Normalize=$false;Gain=0;Position=0;Duration=0;Waveform=0;Queued=0;View=0;Scoped=$false;Peak=0}
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
        elseif ($line -match '^ORDER ([0-9]+)$') { $state.SortOrder=[int]$matches[1] }
        elseif ($line -match '^DRAWN ([0-9]+)$') { $state | Add-Member DrawnTracks ([int]$matches[1]) -Force }
        elseif ($line -match '^PANEL ([0-9]+) ([0-9]+)$') { $state | Add-Member PanelPreferred ([int]$matches[1]) -Force; $state | Add-Member PanelHeight ([int]$matches[2]) -Force }
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
function Wait-Ui([scriptblock]$condition,[string]$description) {
    $timer=[Diagnostics.Stopwatch]::StartNew(); $state=$null
    while ($timer.Elapsed.TotalSeconds -lt 10) {
        if ($hwnd -ne [IntPtr]::Zero -and [FeatureNative]::IsIconic($hwnd)) { [FeatureNative]::ShowWindow($hwnd,9) | Out-Null }
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
function Click([string]$name) {
    $control=Move-Control $name; $before=$control.Clicks
    Send-UiInput 'DOWN'
    try { $null=Wait-Ui {param($s) $s.Controls.ContainsKey($name) -and $s.Controls[$name].Active} "control $name pressed" }
    finally { Send-UiInput 'UP' }
    $null=Wait-Ui {param($s) $s.Activations.ContainsKey($name) -and $s.Activations[$name] -gt $before} "control $name activated"
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
    Click $name; Send-UiInput 'KEY HOME 1'; Send-UiInput 'KEY HOME 0'
    for ($i=0; $i -lt $index; $i++) { Send-UiInput 'KEY DOWN 1'; Send-UiInput 'KEY DOWN 0' }
    Send-UiInput 'KEY ENTER 1'; Send-UiInput 'KEY ENTER 0'
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
    # Establish ImGui window focus before testing a non-interactive text hover.
    Send-UiInput 'MOVE 250 220'
    Send-UiInput 'DOWN'
    Send-UiInput 'UP'
    $info=(Read-Ui).Controls['audio-info']
    Send-UiInput ('MOVE {0} {1}' -f $info.X.ToString([Globalization.CultureInfo]::InvariantCulture),$info.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
    $null=Wait-Ui {param($s) $s.Tooltip -match 'pcm_s16le.*44100 Hz.*1 ch.*Stereo analysis:.*LUFS'} 'audio format and measured loudness tooltip'
    Require ((Get-ChildItem -LiteralPath "$stateDirectory/player-analysis" -Filter '*.bin').Count -eq 2) 'analysis writes its independent per-track cache'
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
    # Add a long catalog only after the existing two-track navigation checks.
    $template=[IO.File]::ReadAllBytes("$fixture/a/same.wav")
    # Real big-endian Sun AU PCM gives file-type grouping/filtering a second format.
    $dataOffset=12
    while ([Text.Encoding]::ASCII.GetString($template,$dataOffset,4) -ne 'data') {
        $length=[BitConverter]::ToInt32($template,$dataOffset+4); $dataOffset+=8+$length+($length%2)
    }
    $pcmOffset=$dataOffset+8; $pcmLength=[BitConverter]::ToInt32($template,$dataOffset+4)
    for ($index=0; $index -lt 120; $index++) {
        $directory=Join-Path $stateDirectory ('media/group-{0:D3}' -f $index)
        if ($index -eq 1) { $directory += '-Long folder 中文 日本語 한국어 👩🏽‍🔬 with a name that needs an ellipsis' }
        if ($index -eq 0) { $directory=Join-Path $directory 'nested' }
        New-Item -ItemType Directory -Force $directory | Out-Null
        $bytes=$template.Clone()
        [BitConverter]::GetBytes([int]($index+1000)).CopyTo($bytes,16384)
        $filename = if ($index -eq 119) { '中文😀 日本語 한국어 👩🏽‍🔬 🇯🇵.wav' } else { 'track-{0:D3}.wav' -f $index }
        if ($index -ge 100 -and $index -lt 119) {
            $filename='track-{0:D3}.au' -f $index
            $au=[byte[]]::new(24+$pcmLength)
            $fields=@(0x2e736e64,24,$pcmLength,3,44100,1)
            for($field=0;$field -lt $fields.Count;$field++) {
                $number=[BitConverter]::GetBytes([int]$fields[$field]);[Array]::Reverse($number);$number.CopyTo($au,$field*4)
            }
            for($sample=0;$sample -lt $pcmLength;$sample+=2){$au[24+$sample]=$bytes[$pcmOffset+$sample+1];$au[25+$sample]=$bytes[$pcmOffset+$sample]}
            $bytes=$au
        }
        [IO.File]::WriteAllBytes((Join-Path $directory $filename),$bytes)
    }
    Click 'Clear'
    Click 'Library'
    Click 'refresh'
    $null=Wait-Ui {param($s) !$s.History -and $s.Visible -eq 122 -and $s.Controls.ContainsKey('refresh') -and $s.Controls['refresh'].Hovered} 'long catalog refresh complete'
    foreach ($mode in @(0,1,2)) {
        Click 'order'
        Send-UiInput 'KEY HOME 1'; Send-UiInput 'KEY HOME 0'
        for ($step=0; $step -lt $mode; $step++) { Send-UiInput 'KEY DOWN 1'; Send-UiInput 'KEY DOWN 0' }
        Send-UiInput 'KEY ENTER 1'; Send-UiInput 'KEY ENTER 0'
        $null=Wait-Ui {param($s) $s.SortOrder -eq $mode -and $s.ScrollMaxY -gt 100} "sort mode $mode selected"
        if ($mode -eq 1) {
            $expandedGroups=Read-Ui
            Require ($null -ne $expandedGroups.DrawnTracks -and $expandedGroups.DrawnTracks -le [Math]::Ceiling($expandedGroups.Height/18)+4) 'Directory groups draw only viewport tracks across 120 folders'
            Click 'group-0'
            $collapsed=Wait-Ui {param($s) $s.ScrollMaxY -lt $expandedGroups.ScrollMaxY-10} 'directory header collapses its track'
            Click 'group-0'
            $null=Wait-Ui {param($s) [Math]::Abs($s.ScrollMaxY-$expandedGroups.ScrollMaxY) -lt 1} 'directory header restores its track'
        }
        Send-UiInput 'MOVE 240 300'
        Send-UiInput 'WHEEL -5'
        $beforeScroll=Wait-Ui {param($s) $s.ScrollY -gt 50} "mode $mode scrolled down"
        Require ($beforeScroll.DrawnTracks -le [Math]::Ceiling($beforeScroll.Height/18)+4) "Only viewport tracks are drawn while scrolling sort mode $mode"
        $row=$beforeScroll.Controls.Keys | Where-Object {$_ -like 'library-*' -and $beforeScroll.Controls[$_].Y -gt 180 -and $beforeScroll.Controls[$_].Y -lt 400} | Sort-Object | Select-Object -First 1
        Require ($null -ne $row) "mode $mode has a visible track below the top"
        $visits=$beforeScroll.Count
        Click $row
        $afterScroll=Wait-Ui {param($s) !$s.Pending -and $s.Count -gt $visits} "mode $mode selected track loaded"
        # ImGui applies a queued scroll target on the following frame.
        Send-UiInput 'MOVE 240 300'
        $afterScroll=Read-Ui
        Require ([Math]::Abs($afterScroll.ScrollY-$beforeScroll.ScrollY) -lt 1) "Starting a track preserves scroll in sort mode $mode"
        Click 'play'
        $null=Wait-Ui {param($s) $s.Paused} "mode $mode playback paused"
    }
    Select-Combo 'order' 1
    $null=Wait-Ui {param($s) $s.Controls.ContainsKey('group-0')} 'directory groups restored before combined checks'
    Click 'group-0'
    Search 'alpha'
    $found=Wait-Ui {param($s) $s.Visible -eq 1} 'search includes tracks inside a collapsed folder'
    Require (@($found.Controls.Keys | Where-Object {$_ -like 'group-*'}).Count -eq 0) 'search shows ranked results without group headers'
    $alphaRow=$found.Controls.Keys | Where-Object {$_ -like 'library-*'} | Select-Object -First 1
    $previousCount=$found.Count; Click $alphaRow
    $null=Wait-Ui {param($s) !$s.Pending -and $s.Count -gt $previousCount -and $s.Visible -eq 1} 'playing a search result preserves the query'
    Click 'play'; $null=Wait-Ui {param($s) $s.Paused} 'search result paused'
    foreach($view in @(1,2,3,0)) {
        Select-Combo 'view' $view
        $expected=if($view -eq 1){0}else{1}
        $null=Wait-Ui {param($s) $s.Visible -eq $expected -and $s.View -eq $view} "search combines with history-derived view $view"
    }
    Click 'history'
    $null=Wait-Ui {param($s) $s.History -and $s.Visible -ge 1} 'query carries into history'
    Click 'Library'
    $null=Wait-Ui {param($s) !$s.History -and $s.Visible -eq 1} 'query carries back to library'
    Click 'Clear'
    $restored=Wait-Ui {param($s) $s.Visible -eq 122 -and $s.Controls.ContainsKey('group-0')} 'clearing search restores directory groups'
    Require (!$restored.Controls.ContainsKey($alphaRow)) 'collapsed directory survives search, playback and history switches'
    Click 'group-0'
    Search 'GROUP-042 TRACK-042'
    $null=Wait-Ui {param($s) $s.Visible -eq 1} 'directory and filename tokens combine ignoring case'
    Select-Combo 'order' 2
    $null=Wait-Ui {param($s) $s.SortOrder -eq 2 -and $s.Visible -eq 1} 'changing grouping preserves active search'
    Search 'no-such-recording-xyz'
    $null=Wait-Ui {param($s) $s.Visible -eq 0 -and $s.DrawnTracks -eq 0} 'empty search results have no stale grouped rows'
    Click 'Clear'
    $null=Wait-Ui {param($s) $s.Controls.ContainsKey('group-0') -and $s.ScrollMaxY -gt 100} 'file-type groups return after empty search'
    Click 'group-0' # AU sorts before WAV.
    $null=Wait-Ui {param($s) $s.Controls.ContainsKey('group-1') -and
        [Math]::Abs($s.Controls['group-1'].Top-$s.Controls['group-0'].Bottom-5) -lt 1} 'AU group collapses independently of WAV'
    Search 'track-10'
    $null=Wait-Ui {param($s) $s.Visible -eq 10} 'search includes tracks inside a collapsed file type'
    Click 'filters'; Select-Combo 'extension' 2; Dismiss-Popup
    $null=Wait-Ui {param($s) $s.Visible -eq 0} 'WAV filter excludes matching AU files'
    Click 'filters'; Select-Combo 'extension' 1; Dismiss-Popup
    $null=Wait-Ui {param($s) $s.Visible -eq 10} 'AU filter restores matching AU files'
    $original="$stateDirectory/media/group-109/track-109.au"; $renamed="$stateDirectory/media/group-109/retitled.au"
    Move-Item -LiteralPath $original -Destination $renamed
    Click 'refresh'
    $null=Wait-Ui {param($s) $s.Visible -eq 9} 'refresh updates matching file identities while searching and filtering'
    Move-Item -LiteralPath $renamed -Destination $original
    Click 'refresh'
    $null=Wait-Ui {param($s) $s.Visible -eq 10} 'restoring a file updates results without clearing search or extension'
    Click 'filters'
    foreach($minimum in @(4,0)) {
        $field=(Wait-Ui {param($s) $s.Controls.ContainsKey('duration-min')} 'combined duration filter rendered').Controls['duration-min']
        Send-UiInput ('MOVE {0} {1}' -f ($field.Left+30).ToString([Globalization.CultureInfo]::InvariantCulture),$field.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
        Send-UiInput 'DOWN'; Send-UiInput 'UP'; Send-UiInput 'KEY CTRL 1'; Send-UiInput 'KEY A 1'; Send-UiInput 'KEY A 0'; Send-UiInput 'KEY CTRL 0'
        Send-UiInput ('TEXT "{0}"' -f $minimum); Send-UiInput 'KEY ENTER 1'; Send-UiInput 'KEY ENTER 0'
        $expected=if($minimum -eq 4){0}else{10}
        $null=Wait-Ui {param($s) $s.Visible -eq $expected} "duration bound $minimum intersects search and extension"
    }
    Click 'measured-only'
    $null=Wait-Ui {param($s) $s.Visible -eq 10} 'measured-duration filter retains analyzed search matches'
    Dismiss-Popup
    Click 'filters'; Select-Combo 'availability' 2; Dismiss-Popup
    $null=Wait-Ui {param($s) $s.Visible -eq 0} 'availability filter intersects search and extension'
    Click 'filters'; Click 'filter-reset'; Dismiss-Popup
    $null=Wait-Ui {param($s) $s.Visible -eq 10} 'resetting filters preserves the active query'
    Click 'Clear'
    $null=Wait-Ui {param($s) $s.Visible -eq 122 -and $s.Controls.ContainsKey('group-0')} 'clearing query restores all file-type groups'
    $null=Wait-Ui {param($s) $s.Controls.ContainsKey('group-1') -and
        [Math]::Abs($s.Controls['group-1'].Top-$s.Controls['group-0'].Bottom-5) -lt 1} 'collapsed file type survives query, filters and refreshes'
    Click 'group-0'
    $null=Wait-Ui {param($s) !$s.Controls.ContainsKey('group-1') -and $s.DrawnTracks -gt 0} 'file type expands after query, filters and refreshes'
    $ready=Wait-Ui {param($s) $s.Waveform -gt 0 -and $s.Paused} 'current-track cached waveform loaded'
    $seek=$ready.Controls['seek']; $x=$seek.Left+($seek.Right-$seek.Left)*0.7
    Send-UiInput ('MOVE {0} {1}' -f $x.ToString([Globalization.CultureInfo]::InvariantCulture),$seek.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
    Send-UiInput 'DOWN'; Send-UiInput 'UP'
    $null=Wait-Ui {param($s) $s.Paused -and $s.Position -gt 1.5 -and $s.Position -lt 2.8} 'waveform mouse seeking preserves pause'
    Send-UiInput 'KEY HOME 1'; Send-UiInput 'KEY HOME 0'
    $null=Wait-Ui {param($s) $s.Position -lt 0.2 -and $s.Paused} 'waveform keyboard Home seeks to start'
    Capture 'waveform'
    Context 'audio-info'
    Require ((Read-Ui).Controls.ContainsKey('copy-path') -and (Read-Ui).Controls.ContainsKey('copy-wsl') -and (Read-Ui).Controls.ContainsKey('reveal')) 'path actions are available on the current track'
    $count=(Read-Ui).Count
    Click 'queue-next'
    $null=Wait-Ui {param($s) $s.Queued -eq 1 -and $s.Count -eq $count} 'Play next schedules without recording a visit'
    Context 'audio-info'; Click 'queue-add'
    $null=Wait-Ui {param($s) $s.Queued -eq 2 -and $s.Count -eq $count} 'Add to queue appends another future slot'
    Click 'next'
    $null=Wait-Ui {param($s) !$s.Pending -and $s.Queued -eq 1 -and $s.Count -eq $count+1} 'Next commits one queued visit'
    Click 'play'; $null=Wait-Ui {param($s) $s.Paused} 'queued playback paused'
    Click 'more'; Capture 'queue'; Dismiss-Popup
    foreach ($view in @(1,2,3,0)) {
        Select-Combo 'view' $view
        $selected=Wait-Ui {param($s) $s.View -eq $view -and !$s.History} "history-derived view $view selected"
        if ($view -eq 1) { Require ($selected.Visible -gt 0 -and $selected.Visible -lt 122) 'Unplayed excludes successful visits' }
        if ($view -eq 2 -or $view -eq 3) { Require ($selected.Visible -gt 0 -and $selected.Visible -lt 122) 'Played view includes retained visits only' }
        Capture "view-$view"
    }
    Click 'search'; Send-UiInput 'TEXT "alpha"'
    $ranked=Wait-Ui {param($s) $s.Visible -eq 1} 'ranked search selects a filename match'
    Capture 'ranked-highlight'
    $rowName=$ranked.Controls.Keys | Where-Object {$_ -like 'library-*'} | Select-Object -First 1
    $row=$ranked.Controls[$rowName]; $origin=[FeatureNative+Point]::new(); $rect=[FeatureNative+Rect]::new()
    [FeatureNative]::ClientToScreen($hwnd,[ref]$origin) | Out-Null; [FeatureNative]::GetWindowRect($hwnd,[ref]$rect) | Out-Null
    $bitmap=[Drawing.Bitmap]::new("$stateDirectory/ranked-highlight.png")
    try {
        $gold=0
        for($x=[int]$row.Left+$origin.X-$rect.Left;$x -lt [int]$row.Right+$origin.X-$rect.Left;$x++) {
            for($y=[int]$row.Top+$origin.Y-$rect.Top;$y -lt [int]$row.Bottom+$origin.Y-$rect.Top;$y++) {
                $pixel=$bitmap.GetPixel($x,$y); if ($pixel.R -gt 180 -and $pixel.G -gt 120 -and $pixel.G -lt 220 -and $pixel.B -lt 150) { $gold++ }
            }
        }
        Require ($gold -gt 5) 'matched token is visibly highlighted in the library row'
    } finally { $bitmap.Dispose() }
    Click 'Clear'; $null=Wait-Ui {param($s) $s.Visible -eq 122} 'ranked search cleared'
    Select-Combo 'order' 3
    $null=Wait-Ui {param($s) $s.SortOrder -eq 3} 'duration sorting selected'
    Click 'filters'; Select-Combo 'availability' 2
    $null=Wait-Ui {param($s) $s.Visible -eq 0} 'offline filter excludes online fixtures'
    Click 'filter-reset'; Dismiss-Popup
    $null=Wait-Ui {param($s) $s.Visible -eq 122} 'filter reset restores library'
    Click 'filters'
    $field=(Wait-Ui {param($s) $s.Controls.ContainsKey('duration-min')} 'duration input rendered').Controls['duration-min']
    Send-UiInput ('MOVE {0} {1}' -f ($field.Left+30).ToString([Globalization.CultureInfo]::InvariantCulture),$field.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
    Send-UiInput 'DOWN'; Send-UiInput 'UP'; Send-UiInput 'KEY CTRL 1'; Send-UiInput 'KEY A 1'; Send-UiInput 'KEY A 0'; Send-UiInput 'KEY CTRL 0'
    Send-UiInput 'TEXT "10"'; Send-UiInput 'KEY ENTER 1'; Send-UiInput 'KEY ENTER 0'
    $null=Wait-Ui {param($s) $s.Visible -eq 0} 'minimum duration filters short and unmeasured tracks'
    Click 'filter-reset'; Dismiss-Popup
    Context 'audio-info'; Click 'repeat-folder'
    $null=Wait-Ui {param($s) !$s.Pending -and $s.Scoped -and $s.Repeat -eq 2} 'context action starts repeating its directory'
    if (!(Read-Ui).Paused) { Click 'play' }
    Click 'more'; Click 'clear-folder'; $null=Wait-Ui {param($s) !$s.Scoped} 'folder scope cleared'
    Check-DirectoryRow 'minimum width'
    Capture 'directory-row-narrow'
    Click 'directory-chip-0'
    $null=Wait-Ui {param($s) $s.Included -eq 1 -and $s.Visible -eq 1} 'priority directory chip includes its subtree'
    Click 'directory-chip-1'
    $null=Wait-Ui {param($s) $s.Included -eq 2 -and $s.Visible -eq 2} 'multiple directory chips combine as a whitelist'
    Check-DirectoryRow 'active filters at minimum width'
    Capture 'directory-row-active'
    $rowBounds=[FeatureNative+Rect]::new(); [FeatureNative]::GetWindowRect($hwnd,[ref]$rowBounds) | Out-Null
    [FeatureNative]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,1000,$rowBounds.Bottom-$rowBounds.Top,0x16) | Out-Null
    $null=Wait-Ui {param($s) $s.Width -gt 900} 'directory row widened'
    Check-DirectoryRow 'wide window'
    [FeatureNative]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,$rowBounds.Right-$rowBounds.Left,$rowBounds.Bottom-$rowBounds.Top,0x16) | Out-Null
    $null=Wait-Ui {param($s) $s.Width -eq 500} 'minimum width restored'
    $beforeResize=Read-Ui; $bounds=[FeatureNative+Rect]::new()
    [FeatureNative]::GetWindowRect($hwnd,[ref]$bounds) | Out-Null
    $frameHeight=$bounds.Bottom-$bounds.Top-$beforeResize.Height
    [FeatureNative]::SetWindowPos($hwnd,[IntPtr]::Zero,0,0,$bounds.Right-$bounds.Left,360+$frameHeight,0x16) | Out-Null
    $null=Wait-Ui {param($s) $s.Height -eq 360 -and !$s.Controls.ContainsKey('directories') -and $s.Controls.ContainsKey('seek')} 'small window keeps browsing controls from overlapping transport'
    Click 'list'
    $null=Wait-Ui {param($s) $s.Height -ge 452 -and $s.Controls.ContainsKey('directories') -and $s.Included -eq 2 -and $s.Visible -eq 2} 'List restores a usable browser size without losing directory selections'
    Search 'track-000'
    $null=Wait-Ui {param($s) $s.Visible -eq 1} 'search intersects a multi-directory whitelist'
    Click 'Clear'
    Click 'directories'; Click 'directory-open-0'
    $null=Wait-Ui {param($s) $s.Controls.ContainsKey('directory-hide-0')} 'directory browser drills into one level'
    Click 'directory-hide-0'
    $null=Wait-Ui {param($s) $s.Hidden -eq 1 -and $s.Visible -eq 1} 'hiding a parent excludes its nested recording'
    Click 'directory-pin-1'
    $null=Wait-Ui {param($s) $s.Pins -eq 1} 'subdirectory can be pinned to the priority row'
    Click 'directory-open-0'; Click 'directory-include-0'
    $null=Wait-Ui {param($s) $s.Included -eq 3 -and $s.Visible -eq 1} 'child inclusion cannot override a hidden ancestor'
    Click 'directory-all'
    $null=Wait-Ui {param($s) $s.Included -eq 0 -and $s.Hidden -eq 1 -and $s.Visible -eq 121} 'clearing temporary includes retains persistent exclusions'
    Capture 'directories'
    Dismiss-Popup
    Click 'directory-overflow'
    $null=Wait-Ui {param($s) $s.Controls.ContainsKey('directory-browse-tree')} 'overflow opens a multi-row priority picker'
    Capture 'directory-overflow'
    Click 'directory-hide-0'
    $null=Wait-Ui {param($s) $s.Hidden -eq 0 -and $s.Visible -eq 122} 'overflow can remove an active exclusion'
    Click 'directory-browse-tree'; Click 'directory-open-0'; Click 'directory-hide-0'
    $null=Wait-Ui {param($s) $s.Hidden -eq 1 -and $s.Visible -eq 121} 'overflow picker can return to tree browsing'
    Dismiss-Popup
    Click 'directory-chip-0'
    $null=Wait-Ui {param($s) $s.Hidden -eq 0 -and $s.Visible -eq 122} 'active hidden chip restores its subtree without adding an include'
    Click 'directory-overflow'; Click 'directory-hide-1'
    $null=Wait-Ui {param($s) $s.Hidden -eq 1 -and $s.Included -eq 0 -and $s.Visible -eq 121} 'overflow restores the exclusion after pinned suggestions reorder'
    Dismiss-Popup
    Click 'directory-chip-1'
    $null=Wait-Ui {param($s) $s.Included -eq 1 -and $s.Visible -eq 1} 'active exclusion precedes the pinned suggestion'
    $divider=Move-Control 'transport-resize'; $position=(Read-Ui).Position
    Send-UiInput ('MOVE {0} {1}' -f $divider.X.ToString([Globalization.CultureInfo]::InvariantCulture),$divider.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
    Send-UiInput 'DOWN'
    Send-UiInput ('MOVE {0} {1}' -f $divider.X.ToString([Globalization.CultureInfo]::InvariantCulture),($divider.Y-110).ToString([Globalization.CultureInfo]::InvariantCulture))
    Send-UiInput 'UP'
    $raised=Wait-Ui {param($s) $s.PanelPreferred -ge 310 -and $s.Controls['seek'].Bottom-$s.Controls['seek'].Top -gt 130} 'dragging transport upward enlarges the waveform'
    Capture 'transport-expanded'
    Require ($raised.Paused -and [Math]::Abs($raised.Position-$position) -lt .01) 'resizing transport does not seek or resume playback'
    $null=Move-Control 'transport-resize'; Send-UiInput 'DOUBLE_CLICK'
    $reset=Wait-Ui {param($s) $s.PanelPreferred -eq 202 -and $s.Controls['seek'].Bottom-$s.Controls['seek'].Top -eq 26 -and
        [Math]::Abs($s.Controls['transport-resize'].Y-($s.Height-$s.PanelHeight-3)) -lt 1} 'double-click restores compact waveform height'
    Require ($reset.Paused -and [Math]::Abs($reset.Position-$position) -lt .01 -and $reset.Count -eq $raised.Count) 'double-click reset does not seek, play or select an underlying row'
    $divider=Move-Control 'transport-resize'
    Send-UiInput ('MOVE {0} {1}' -f $divider.X.ToString([Globalization.CultureInfo]::InvariantCulture),$divider.Y.ToString([Globalization.CultureInfo]::InvariantCulture))
    Send-UiInput 'DOWN'
    Send-UiInput ('MOVE {0} {1}' -f $divider.X.ToString([Globalization.CultureInfo]::InvariantCulture),($divider.Y-110).ToString([Globalization.CultureInfo]::InvariantCulture))
    Send-UiInput 'UP'
    $null=Wait-Ui {param($s) $s.PanelPreferred -ge 310} 'custom waveform height prepared for restart'
    Click 'more'; Click 'topmost'
    $null=Wait-Ui {param($s) $s.Topmost} 'always-on-top preference enabled'
    Require (([FeatureNative]::GetWindowLongPtr($hwnd,-20).ToInt64() -band 8) -ne 0) 'native always-on-top style applied'
    Click 'normalize'
    $null=Wait-Ui {param($s) $s.Normalize -and $s.Gain -gt 10} 'measured normalization applies gain without replacing volume'
    Click 'strip'
    $null=Wait-Ui {param($s) $s.Strip -and $s.Height -eq 86 -and $s.Waveform -gt 0} 'slim strip retains cached waveform and transport'
    Capture 'strip'
    Write-Host "Feature screenshots: $stateDirectory"
} finally {
    if (!$process.HasExited) {
        if ($hwnd -ne [IntPtr]::Zero) { [FeatureNative]::PostMessage($hwnd,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
        if (!$process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force; throw 'Feature test player did not close promptly.' }
    }
}
Require ($process.ExitCode -eq 0) 'Feature test clean shutdown'
if ($env:PRAPPY_LEAK_REPORT -and $BuildVariant.StartsWith('Debug') -and !$BuildVariant.Contains('Asan')) {
    Require ((Get-Content -LiteralPath $env:PRAPPY_LEAK_REPORT -Raw) -match '^PASS no CRT leaks\s*$') 'first feature instance has no CRT leaks'
    Copy-Item -LiteralPath $env:PRAPPY_LEAK_REPORT -Destination "$env:PRAPPY_LEAK_REPORT.first-instance.log"
}
$script:inputSequence=0
Remove-Item -LiteralPath "$stateDirectory/ui-command.txt","$stateDirectory/ui-status.txt" -ErrorAction SilentlyContinue
$process=Start-Process -FilePath "$app/build/$BuildVariant/PrappyPlayer.exe" -WindowStyle Hidden -PassThru -ArgumentList '--window-test',('"{0}"' -f $stateDirectory)
try {
    $timer=[Diagnostics.Stopwatch]::StartNew()
    do { $process.Refresh(); $hwnd=$process.MainWindowHandle; if ($hwnd -ne [IntPtr]::Zero) { break }; Start-Sleep -Milliseconds 10 } while ($timer.Elapsed.TotalSeconds -lt 10)
    $null=Wait-Ui {param($s) $s.Strip -and $s.Topmost -and $s.Normalize -and $s.Paused -and $s.Queued -eq 1 -and $s.Waveform -gt 0} 'strip/topmost/normalization/queue restore together paused'
    Require (([FeatureNative]::GetWindowLongPtr($hwnd,-20).ToInt64() -band 8) -ne 0) 'native topmost restores after restart'
    $restored=Read-Ui
    Require ($restored.Included -eq 0 -and $restored.Hidden -eq 1 -and $restored.Pins -eq 1 -and $restored.PanelPreferred -ge 310) 'height, hidden directories and pins restore; includes remain temporary'
    Send-UiInput 'KEY CTRL 1'; Send-UiInput 'KEY SHIFT 1'; Send-UiInput 'KEY M 1'; Send-UiInput 'KEY M 0'; Send-UiInput 'KEY SHIFT 0'; Send-UiInput 'KEY CTRL 0'
    $compact=Wait-Ui {param($s) !$s.Strip -and $s.Height -ge 250} 'strip keyboard shortcut restores normal layout'
    Require ($compact.PanelPreferred -ge 310 -and $compact.PanelHeight -eq 202) 'small window clamps the panel without losing the preferred height'
    Send-UiInput 'KEY CTRL 1'; Send-UiInput 'KEY SHIFT 1'; Send-UiInput 'KEY T 1'; Send-UiInput 'KEY T 0'; Send-UiInput 'KEY SHIFT 0'; Send-UiInput 'KEY CTRL 0'
    $null=Wait-Ui {param($s) !$s.Topmost} 'topmost keyboard shortcut toggles preference'
    Require (([FeatureNative]::GetWindowLongPtr($hwnd,-20).ToInt64() -band 8) -eq 0) 'topmost shortcut clears native style'
    Click 'list'
    $null=Wait-Ui {param($s) $s.PanelHeight -ge 310 -and $s.Visible -eq 121} 'expanded window restores saved waveform height and excludes hidden subtree'
    $null=Move-Control 'transport-resize'; Send-UiInput 'DOUBLE_CLICK'
    $null=Wait-Ui {param($s) $s.PanelPreferred -eq 202} 'restored panel supports double-click reset'
    Click 'search'
    $null=Wait-Ui {param($s) $s.Controls['search'].Active} 'search is active before SDL text events'
    Send-UiInput 'NATIVE_EDIT "中"'
    $null=Wait-Ui {param($s) $s.Composing} 'SDL composition reaches the player'
    foreach($key in @('ENTER','ESC','SPACE')) {
        Send-UiInput "NATIVE_KEY $key 1"; Send-UiInput "NATIVE_KEY $key 0"
    }
    Require ((Read-Ui).Paused) 'composition keys preserve paused playback'
    Send-UiInput 'NATIVE_TEXT "中文😀"'; Send-UiInput 'NATIVE_EDIT ""'
    $null=Wait-Ui {param($s) !$s.Composing -and $s.Visible -eq 1} 'native Unicode commit finds multilingual filename'
    Capture 'unicode'
    Click 'Clear'
    $null=Wait-Ui {param($s) $s.Visible -gt 0} 'Unicode search clears normally'
    Send-UiInput 'MOVE 0 0'
    Start-Sleep -Seconds 2
    $first=(Read-Ui).Presents
    Start-Sleep -Seconds 2
    Require ((Read-Ui).Presents -eq $first) 'paused UI presents no idle frames after settling'
} finally {
    if (!$process.HasExited) { $null=$process.CloseMainWindow(); if (!$process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force; throw 'Restored feature player did not close.' } }
}
Require ($process.ExitCode -eq 0) 'Restored feature player clean shutdown'
