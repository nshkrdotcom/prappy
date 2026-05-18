#Requires -Version 7.0
param(
    [Parameter()]
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",

    [Parameter()]
    [switch]$SmokeTest
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_tooling.ps1"

$parameters = @{
    Config = $Config
}

if ($SmokeTest) {
    $parameters.SmokeTest = $true
}

Invoke-PrappyNativeTool "run.ps1" $parameters
