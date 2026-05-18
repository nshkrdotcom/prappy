#Requires -Version 7.0
param(
    [Parameter()]
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",

    [Parameter()]
    [ValidateSet("RandomLines", "Starfield", "Oahu", "ParticleField")]
    [string]$Visualization,

    [Parameter()]
    [ValidateSet("Auto", "D3D11", "D3D12", "Vulkan")]
    [string]$Renderer = "Auto",

    [Parameter()]
    [ValidateSet("Default", "All", "Coastline", "Mesh", "Landmarks", "Flyover")]
    [string]$OahuDiagnostic = "Default",

    [Parameter()]
    [switch]$OahuTopDown,

    [Parameter()]
    [switch]$Focus,

    [Parameter()]
    [switch]$NoOverlay,

    [Parameter()]
    [switch]$SmokeTest,

    [Parameter()]
    [switch]$ScreenshotSmoke
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_tooling.ps1"

$parameters = @{
    Config = $Config
    Renderer = $Renderer
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

if ($OahuDiagnostic -ne "Default") {
    $parameters.OahuDiagnostic = $OahuDiagnostic
}

if ($OahuTopDown) {
    $parameters.OahuTopDown = $true
}

if ($Focus) {
    $parameters.Focus = $true
}

if ($NoOverlay) {
    $parameters.NoOverlay = $true
}

Invoke-PrappyNativeTool "run.ps1" $parameters
