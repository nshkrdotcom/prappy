#Requires -Version 7.0
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$test=Join-Path $app 'build\dedup-test\scraper\library'
foreach($folder in @('a','b','copies')) { New-Item -ItemType Directory -Force "$test\$folder" | Out-Null }
# Two valid WAVs share filename, length, header and tail, but differ in the middle.
$stream=[IO.File]::Create("$test\a\same.wav")
$writer=[IO.BinaryWriter]::new($stream)
try {
    $length=44100*3*2
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $writer.Write([uint32]($length+36))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $writer.Write([uint32]16)
    $writer.Write([uint16]1); $writer.Write([uint16]1); $writer.Write([uint32]44100)
    $writer.Write([uint32]88200); $writer.Write([uint16]2); $writer.Write([uint16]16)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([uint32]$length)
    for($i=0;$i -lt 44100*3;$i++) { $writer.Write([int16](1000*[Math]::Sin($i*2*[Math]::PI*440/44100))) }
} finally { $writer.Dispose() }
$bytes=[IO.File]::ReadAllBytes("$test\a\same.wav")
$bytes[16384]=$bytes[16384] -bxor 1
[IO.File]::WriteAllBytes("$test\b\same.wav",$bytes)
Copy-Item "$test\a\same.wav" "$test\copies\renamed.wav" -Force
if (Test-Path -LiteralPath "$test\copies\another.wav") { Remove-Item -LiteralPath "$test\copies\another.wav" }
New-Item -ItemType HardLink -Path "$test\copies\another.wav" -Target "$test\copies\renamed.wav" | Out-Null
@"
directory=$test
directory=$test\copies
extensions=wav
"@ | Set-Content "$test\player.ini" -Encoding utf8
$p=Start-Process -FilePath "$app\build\Release\PrappyPlayer.exe" -WindowStyle Hidden -Wait -PassThru `
    -ArgumentList '--self-test',"$test\player.ini","$test\results.log"
$results=Get-Content "$test\results.log" -Raw
Write-Host $results
if($p.ExitCode -ne 0 -or $results -notmatch 'Scan: 2 tracks \| 2 identical copies hidden') {
    throw 'Dedup regression failed: renamed copies must collapse; same-name middle differences must remain.'
}
if ($results -notmatch 'Testing library\\a\\same\.wav') { throw 'Track label did not strip scraper and its parent directories.' }
Write-Host 'PASS renamed copies, overlapping roots, same-name distinct audio, and equal-sample/different-middle collision.'
