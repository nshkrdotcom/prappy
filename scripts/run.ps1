#Requires -Version 7.0
param(
    [Parameter()]
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",

    [Parameter()]
    [ValidateSet("RandomLines", "Starfield", "Oahu")]
    [string]$Visualization,

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

if ($Visualization) {
    $parameters.Visualization = $Visualization
}

Invoke-PrappyNativeTool "run.ps1" $parameters
