#Requires -Version 7.0
param(
    [Parameter()]
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",

    [Parameter()]
    [ValidateSet("RandomLines", "Starfield", "Oahu")]
    [string]$Visualization,

    [Parameter()]
    [switch]$SmokeTest,

    [Parameter()]
    [switch]$ScreenshotSmoke
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_tooling.ps1"

$parameters = @{
    Config = $Config
}

if ($SmokeTest) {
    $parameters.SmokeTest = $true
}

if ($ScreenshotSmoke) {
    $parameters.ScreenshotSmoke = $true
}

if ($Visualization) {
    $parameters.Visualization = $Visualization
}

Invoke-PrappyNativeTool "run.ps1" $parameters
