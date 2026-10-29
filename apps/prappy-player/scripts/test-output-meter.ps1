#Requires -Version 7.0
param([string]$BuildVariant='Release')
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$directory=Join-Path $app 'build/output-meter-test'
New-Item -ItemType Directory -Force $directory | Out-Null
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
public static class PrappyMeterFixture {
    [DllImport("winmm.dll",CharSet=CharSet.Unicode)]
    public static extern bool PlaySound(string file,IntPtr module,uint flags);
    public static void Write(string file,bool alternating,double frequency) {
        const int frames=48000*20;
        using(var writer=new BinaryWriter(File.Create(file))) {
            writer.Write(System.Text.Encoding.ASCII.GetBytes("RIFF"));writer.Write(36+frames*4);
            writer.Write(System.Text.Encoding.ASCII.GetBytes("WAVEfmt "));writer.Write(16);
            writer.Write((short)1);writer.Write((short)2);writer.Write(48000);writer.Write(192000);
            writer.Write((short)4);writer.Write((short)16);
            writer.Write(System.Text.Encoding.ASCII.GetBytes("data"));writer.Write(frames*4);
            for(int f=0;f<frames;f++) {
                int second=f/48000;
                short sample=(short)((!alternating||second>=6||second%2!=0)?8000*Math.Sin(2*Math.PI*frequency*f/48000):0);
                writer.Write(sample);writer.Write(sample);
            }
        }
    }
}
'@
Add-Type -Path "$PSScriptRoot/AudioLoopback.cs"
$signal=Join-Path $directory 'alternating.wav'
$foreign=Join-Path $directory 'other-process.wav'
$log=Join-Path $directory "meter-$BuildVariant.log"
[PrappyMeterFixture]::Write($signal,$true,660)
[PrappyMeterFixture]::Write($foreign,$false,330)
$capture=$null
try {
    # This process emits an unrelated continuous tone while the player tests its
    # silence intervals. Accidentally metering the whole endpoint must fail.
    if (![PrappyMeterFixture]::PlaySound($foreign,[IntPtr]::Zero,0x20009)) { throw 'Cannot start other-process test tone.' }
    $capture=[PrappyAudioLoopback]::new(660)
    $process=Start-Process -FilePath "$app/build/$BuildVariant/PrappyPlayer.exe" -WindowStyle Hidden -PassThru `
        -ArgumentList '--meter-test',('"{0}"' -f $signal),('"{0}"' -f $log)
    if (!$process.WaitForExit(30000)) { Stop-Process -Id $process.Id -Force; throw 'Output meter test timed out.' }
    Get-Content -LiteralPath $log
    if ($process.ExitCode -ne 0 -or (Get-Content -LiteralPath $log -Raw) -notmatch 'ALL PASS') { throw 'Output meter regression failed.' }
    $amplitude=$capture.ToneAmplitude
    Write-Host "WASAPI loopback: 660 Hz amplitude=$amplitude frames=$($capture.Frames)"
    if ($capture.Frames -lt 48000 -or $amplitude -lt 0.00001) { throw 'Expected player tone absent from actual Windows output.' }
    Write-Host 'PASS output timing, application-only isolation and real output capture'
} finally {
    $null=[PrappyMeterFixture]::PlaySound($null,[IntPtr]::Zero,0)
    if ($capture) { $capture.Dispose() }
}
