#Requires -Version 7.0
param([string]$Root=(Join-Path $PSScriptRoot '../build/reporting-deps'))
$ErrorActionPreference='Stop'
$Root=[IO.Path]::GetFullPath($Root)
$null=New-Item -ItemType Directory -Force -Path $Root
$dependencies=@(
    @{Name='boost_1_91_0';Url='https://archives.boost.io/release/1.91.0/source/boost_1_91_0.tar.gz';Hash='5734305F40A76C30F951C9ABD409A45A2A19FB546EFE4162119250BBE4D3A463';Strip=$false},
    @{Name='sentry-native';Url='https://codeload.github.com/getsentry/sentry-native/tar.gz/7afc37457d7fcc37fb0bace9c8045d3fa687717f';Hash='6B0E5EC8977748A27E24F1FCFF8E754B809157FC17F47645AD723198D71693E8';Strip=$true}
)
foreach($dependency in $dependencies){
    $destination=Join-Path $Root $dependency.Name
    $receipt=Join-Path $destination '.prappy-dependency-sha256'
    if((Test-Path -LiteralPath $receipt) -and (Get-Content -LiteralPath $receipt -Raw).Trim() -eq $dependency.Hash){continue}
    $archive=Join-Path $Root ($dependency.Name+'.tar.gz')
    if(!(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive).Hash -ne $dependency.Hash){Invoke-WebRequest $dependency.Url -OutFile $archive}
    if((Get-FileHash -LiteralPath $archive).Hash -ne $dependency.Hash){throw "Reporting dependency checksum mismatch: $($dependency.Name)"}
    if($dependency.Strip){
        $null=New-Item -ItemType Directory -Force -Path $destination
        & tar -xf $archive -C $destination --strip-components=1
    }else{& tar -xf $archive -C $Root}
    if($LASTEXITCODE -ne 0){throw "Reporting dependency extraction failed: $($dependency.Name)"}
    Set-Content -LiteralPath $receipt -Value $dependency.Hash
}
Write-Host 'Pinned Boost.Stacktrace 1.91.0 and Sentry Native 0.17.6 reporting sources ready.'
