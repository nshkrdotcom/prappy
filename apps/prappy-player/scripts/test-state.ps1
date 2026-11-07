#Requires -Version 7.0
param([string]$BuildVariant='Release')
$ErrorActionPreference='Stop'
$app = Split-Path $PSScriptRoot -Parent
$stateOutput = Join-Path $app ('build/state-tests/'+$BuildVariant)
New-Item -ItemType Directory -Force $stateOutput | Out-Null
$fixture=Join-Path $stateOutput 'media'
$null=New-Item -ItemType Directory -Force $fixture
foreach($name in @('first','second')){
    $writer=[IO.BinaryWriter]::new([IO.File]::Create("$fixture/$name.wav"))
    try{
        $length=44100*3*2
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'));$writer.Write([uint32]($length+36))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '));$writer.Write([uint32]16)
        $writer.Write([uint16]1);$writer.Write([uint16]1);$writer.Write([uint32]44100)
        $writer.Write([uint32]88200);$writer.Write([uint16]2);$writer.Write([uint16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data'));$writer.Write([uint32]$length)
        $frequency=if($name-eq 'first'){440}else{550}
        for($i=0;$i-lt 44100*3;$i++){$writer.Write([int16](1000*[Math]::Sin($i*2*[Math]::PI*$frequency/44100)))}
    }finally{$writer.Dispose()}
}
[IO.File]::WriteAllText("$fixture/player.ini","debug_log=true",[Text.UTF8Encoding]::new($false))
$output = Join-Path $stateOutput "state-$BuildVariant.log"
$process = Start-Process -FilePath "$app/build/$BuildVariant/tests/PrappyStateTests.exe" -WindowStyle Hidden -PassThru `
    -RedirectStandardError "$stateOutput/state-stderr-$BuildVariant.log" `
    -ArgumentList '--state-test',('"{0}"' -f "$fixture/player.ini"),('"{0}"' -f $output)
if (!$process.WaitForExit(120000)) { Stop-Process -Id $process.Id -Force; throw 'State regression timed out.' }
$result = Get-Content -LiteralPath $output -Raw
Write-Host $result
if ($process.ExitCode -ne 0 -or $result -notmatch 'ALL PASS') { throw 'History/catalog/session/repeat regression failed.' }
