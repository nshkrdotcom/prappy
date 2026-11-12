#Requires -Version 7.0
param([string]$BuildVariant='Debug')
$ErrorActionPreference='Stop'
$app=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build=Join-Path $app "build/$BuildVariant"
$root=Join-Path $app 'build/reporting-validation'
$null=New-Item -ItemType Directory -Force -Path "$root/incidents"
$manifest=Get-Content -LiteralPath "$build/player-build.json" -Raw | ConvertFrom-Json
$symbolRoot=Join-Path $env:LOCALAPPDATA 'Prappy/Reports'
$resultFile=Join-Path $root "results-$BuildVariant.txt"
$results=[Collections.Generic.List[string]]::new()
$artifacts=[ordered]@{}
$activeProbe=$null
function Stop-TestCollector {
    try{$health=Invoke-RestMethod 'http://127.0.0.1:19877/health' -TimeoutSec 2}catch{return}
    if([IO.Path]::GetFullPath($health.root) -ne $root){throw 'Port 19877 belongs to a different test root; refusing to stop it.'}
    Invoke-RestMethod -Method Post 'http://127.0.0.1:19877/shutdown' -TimeoutSec 2 | Out-Null
    $deadline=(Get-Date).AddSeconds(5)
    while((Get-NetTCPConnection -LocalPort 19877 -State Listen -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline){Start-Sleep -Milliseconds 100}
}
try {
    Stop-TestCollector
    foreach($kind in @('handled','third-party','nested','unknown','device-lost','access-violation','assert','abort','terminate','worker','normal','modal','phase-load','imgui','hang')){
        $before=@(Get-ChildItem -LiteralPath "$root/incidents" -Directory | Select-Object -ExpandProperty Name)
        if($kind -eq 'device-lost'){
            $state=Join-Path $root "device-lost-$BuildVariant"
            $null=New-Item -ItemType Directory -Force -Path $state
            Set-Content -LiteralPath "$state/player.ini" -Value 'debug_log=false'
            Set-Content -LiteralPath "$state/test-token" -Value 'disposable-test-token'
            @{url='http://127.0.0.1:9';token_file=(Join-Path $state 'test-token')}|ConvertTo-Json|Set-Content -LiteralPath "$state/sampling-connection.json"
            $oldProbe=$env:PRAPPY_TEST_CRASH;$env:PRAPPY_TEST_CRASH='device-lost'
            try{$activeProbe=Start-Process -FilePath "$build/PrappyPlayer.exe" -ArgumentList '--window-test',('"'+$state+'"') -WindowStyle Hidden -PassThru -RedirectStandardError "$root/$BuildVariant-$kind-stderr.txt"}
            finally{if($null -eq $oldProbe){Remove-Item Env:PRAPPY_TEST_CRASH}else{$env:PRAPPY_TEST_CRASH=$oldProbe}}
        }else{
            $activeProbe=Start-Process -FilePath "$build/tests/prappy_crash_probe.exe" -ArgumentList ('"'+$build+'\PrappyPlayer.exe"'),('"'+$root+'"'),$kind -WindowStyle Hidden -PassThru -RedirectStandardError "$root/$BuildVariant-$kind-stderr.txt"
        }
        $probeHandle=$activeProbe.Handle
        if($kind -eq 'hang'){
            $deadline=(Get-Date).AddSeconds(25)
            do{Start-Sleep -Milliseconds 200;$new=@(Get-ChildItem -LiteralPath "$root/incidents" -Directory | Where-Object {$_.Name -notin $before -and (Test-Path -LiteralPath (Join-Path $_.FullName 'event.json'))})}while(!$new.Count -and (Get-Date) -lt $deadline)
            if(!$new.Count){throw 'Hang watchdog did not deliver a report.'}
            $snapshot=Join-Path $root ("full-snapshot-$BuildVariant-"+[guid]::NewGuid().ToString('N'))
            & "$build/PrappyReports.exe" --snapshot $activeProbe.Id $snapshot *> "$root/full-snapshot-$BuildVariant.txt"
            if($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath "$snapshot/crash.dmp")){throw 'External full-memory snapshot failed.'}
            $results.Add('PASS external full-memory snapshot of a hung process')
            Stop-Process -Id $activeProbe.Id -Force
            $activeProbe.WaitForExit()
        }elseif(!$activeProbe.WaitForExit(20000)){throw "Crash probe timed out: $kind PID $($activeProbe.Id)"}
        $exit=$activeProbe.ExitCode;$activeProbe=$null
        $new=@(Get-ChildItem -LiteralPath "$root/incidents" -Directory | Where-Object {$_.Name -notin $before})
        if($kind -in @('normal','modal','phase-load')){
            if($exit -ne 0 -or $new.Count){throw 'Normal close produced a failure incident or nonzero status.'}
            $results.Add("PASS $kind produces no crash incident");continue
        }
        if($new.Count -ne 1){throw "Expected one incident for $kind, got $($new.Count)"}
        $folder=$new[0].FullName
        $artifacts[$kind]=$folder
        if(!(Test-Path -LiteralPath "$folder/event.json") -or !(Test-Path -LiteralPath "$folder/report.envelope")){throw "Incomplete collected incident: $kind"}
        $event=Get-Content -LiteralPath "$folder/event.json" -Raw | ConvertFrom-Json
        if($event.tags.build_id -ne $manifest.build_id){throw "Build identity missing from $kind event."}
        if($kind -in @('handled','third-party','nested','unknown','device-lost')){
            if($exit -ne 1){throw "Handled failure exit status incorrect: $kind"}
            $fatal=Get-Content -LiteralPath "$folder/fatal.json" -Raw | ConvertFrom-Json
            $source=if($kind -eq 'device-lost'){'player_ui.cpp'}else{'crash_reporting.cpp'}
            if($fatal.stack_origin -ne 'initial throw' -or $fatal.stacktrace -notmatch ($source+':\d+')){throw "Original source location missing for $kind"}
            if($kind -eq 'device-lost' -and $fatal.exceptions[0].message -notmatch 'GetDeviceRemovedReason='){throw 'Device-loss report omitted the actual D3D device reason.'}
            if($kind -eq 'handled' -and $fatal.stacktrace -notmatch 'probeThrowSite'){throw 'Original named throw function missing.'}
            if($kind -eq 'third-party' -and $fatal.exceptions[0].type -notmatch 'nlohmann'){throw 'Third-party exception identity missing.'}
            if($kind -eq 'third-party' -and $manifest.configuration -eq 'Debug' -and $fatal.stacktrace -notmatch 'json.hpp:\d+'){throw 'Third-party source location missing.'}
            if($kind -eq 'nested' -and $fatal.exceptions.Count -ne 2){throw 'Nested exception chain missing.'}
            if($kind -ne 'device-lost' -and $event.breadcrumbs.message -notcontains 'Recorded even with debug_log=false'){throw 'Breadcrumbs were lost with debug logging disabled.'}
        }elseif($kind -notin @('imgui','hang')){
            if(!(Test-Path -LiteralPath "$folder/crash.dmp")){throw "Native dump missing for $kind"}
            if(!(Test-Path -LiteralPath "$folder/phase.txt") -or (Get-Content "$folder/phase.txt" -Raw) -notmatch 'crash-probe'){throw 'Native crash lost its latest mapped UI phase.'}
            if($kind -eq 'assert' -and (Get-Content "$folder/emergency.txt" -Raw) -notmatch 'injected assertion.*crash_reporting.cpp'){throw 'Assertion identity was not durably recorded.'}
            & "$build/PrappyReports.exe" --analyze $folder $symbolRoot *> "$folder/analysis.txt"
            if($LASTEXITCODE -ne 0){throw "Dump analysis failed for $kind"}
            $analysis=Get-Content -LiteralPath "$folder/analysis.txt" -Raw
            if($analysis -notmatch 'private pdb symbols' -or $analysis -notmatch 'crash_reporting.cpp @ \d+'){throw "Matching symbols/source line missing from $kind native dump."}
            if($analysis -notmatch 'Failure location:.*crash_reporting.cpp:\d+'){throw "Concise failure location missing from $kind analysis."}
        }elseif($kind -eq 'hang'){
            if($event.exception.values.type -notcontains 'AppHang'){throw 'Hang report has no AppHang category.'}
            & "$build/PrappyReports.exe" --analyze $folder $symbolRoot *> "$folder/analysis.txt"
            if($LASTEXITCODE -ne 0 -or (Get-Content "$folder/analysis.txt" -Raw) -notmatch 'reporting::probe.*crash_reporting.cpp:\d+'){throw 'Hang report did not resolve the blocked application source.'}
        }
        $results.Add("PASS $kind collected locally with build identity and failure context")
    }
    if(Test-Path -LiteralPath "$root/disabled-debug.log"){throw 'Probe unexpectedly enabled the optional debug log.'}
    # An invalid report destination must fail visibly on stderr without a dialog.
    $blocked=Join-Path $root 'blocked-destination';Set-Content -LiteralPath $blocked -Value 'This file is deliberately not a directory.'
    $activeProbe=Start-Process -FilePath "$build/tests/prappy_crash_probe.exe" -ArgumentList ('"'+$build+'\PrappyPlayer.exe"'),('"'+$blocked+'"'),'handled' -WindowStyle Hidden -PassThru -RedirectStandardError "$root/$BuildVariant-unwritable-stderr.txt"
    $probeHandle=$activeProbe.Handle
    if(!$activeProbe.WaitForExit(10000) -or $activeProbe.ExitCode -ne 1){throw 'Unwritable report destination did not fail cleanly.'}
    $activeProbe=$null
    if(!(Get-Content "$root/$BuildVariant-unwritable-stderr.txt" -Raw)){throw 'Unwritable destination had no visible diagnostic.'}
    $results.Add('PASS unwritable destination fails visibly without hanging')
    # The same report can be retried without losing its original durable bytes.
    $envelopePath=Join-Path $artifacts.handled 'report.envelope'
    $beforeHash=(Get-FileHash -LiteralPath $envelopePath).Hash
    Invoke-WebRequest -Method Post 'http://127.0.0.1:19877/api/1/envelope/' -ContentType 'application/x-sentry-envelope' -InFile $envelopePath -TimeoutSec 5 | Out-Null
    if((Get-FileHash -LiteralPath $envelopePath).Hash -ne $beforeHash){throw 'Collector retry altered the envelope.'}
    $results.Add('PASS collector accepts idempotent retries')
    $retryId=[guid]::NewGuid().ToString('N')
    $retryFolder=Join-Path $root "incidents/$retryId"
    $null=New-Item -ItemType Directory -Force -Path $retryFolder
    $retryBody=(@{event_id=$retryId}|ConvertTo-Json -Compress)+"`n"+'{"type":"event"}'+"`n"+(@{event_id=$retryId;message='transient storage failure probe'}|ConvertTo-Json -Compress)+"`n"
    $lockedFile=[IO.File]::Open((Join-Path $retryFolder 'report.envelope.tmp'),[IO.FileMode]::Create,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try{
        $response=Invoke-WebRequest -Method Post 'http://127.0.0.1:19877/api/1/envelope/' -ContentType 'application/x-sentry-envelope' -Body $retryBody -SkipHttpErrorCheck -TimeoutSec 5
        if($response.StatusCode -ne 503){throw 'Transient storage failure was not marked retryable.'}
        if(!(Invoke-RestMethod 'http://127.0.0.1:19877/health').last_error){throw 'Storage failure missing from collector health.'}
    }finally{$lockedFile.Dispose()}
    Invoke-WebRequest -Method Post 'http://127.0.0.1:19877/api/1/envelope/' -ContentType 'application/x-sentry-envelope' -Body $retryBody -TimeoutSec 5 | Out-Null
    if(!(Test-Path -LiteralPath "$retryFolder/report.envelope")){throw 'Collector did not recover from temporary storage failure.'}
    $results.Add('PASS transient storage failures are visible, retryable and recoverable')
    $failure=Get-Item -LiteralPath $artifacts.handled
    $retained=$failure.FullName
    # Normal restarts must not displace the last failure (old ProcDump behavior).
    for($i=0;$i -lt 3;$i++){
        $activeProbe=Start-Process -FilePath "$build/tests/prappy_crash_probe.exe" -ArgumentList ('"'+$build+'\PrappyPlayer.exe"'),('"'+$root+'"'),'normal' -WindowStyle Hidden -PassThru
        $probeHandle=$activeProbe.Handle
        if(!$activeProbe.WaitForExit(10000) -or $activeProbe.ExitCode -ne 0){throw 'Normal restart failed.'}
        $activeProbe=$null
    }
    if(!(Test-Path -LiteralPath $retained)){throw 'Normal restarts removed the retained failure.'}
    $results.Add('PASS normal restarts retain existing failure evidence')
    $verified=Join-Path $root "verified-$BuildVariant"
    $null=New-Item -ItemType Directory -Force -Path $verified
    foreach($kind in @($artifacts.Keys)){
        $destination=Join-Path $verified $kind
        $null=New-Item -ItemType Directory -Force -Path $destination
        Get-ChildItem -LiteralPath $artifacts[$kind] -File | Copy-Item -Destination $destination -Force
        $artifacts[$kind]=$destination
    }
    # Test count retention in the disposable test store only.
    for($i=0;$i -lt 52;$i++){
        $id=[guid]::NewGuid().ToString('N')
        $body=(@{event_id=$id}|ConvertTo-Json -Compress)+"`n"+'{"type":"event"}'+"`n"+(@{event_id=$id;message='retention test'}|ConvertTo-Json -Compress)+"`n"
        Invoke-WebRequest -Method Post 'http://127.0.0.1:19877/api/1/envelope/' -ContentType 'application/x-sentry-envelope' -Body $body -TimeoutSec 5 | Out-Null
    }
    if(@(Get-ChildItem -LiteralPath "$root/incidents" -Directory).Count -gt 50){throw 'Collector exceeded incident retention count.'}
    $results.Add('PASS incident retention is bounded to 50 reports')
    $results.Add('ALL PASS')
}finally{
    if($activeProbe -and !$activeProbe.HasExited){Stop-Process -Id $activeProbe.Id -Force}
    Stop-TestCollector
    $results | Set-Content -LiteralPath $resultFile
    $artifacts | ConvertTo-Json | Set-Content -LiteralPath "$root/artifacts-$BuildVariant.json"
    $results | Write-Output
}
