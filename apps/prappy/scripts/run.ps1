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
    [ValidateSet("Default", "RandomLinesHero", "StarfieldHero", "OahuFlyover", "OahuCenteredTopDown", "OahuDebugMesh", "ParticlesHero")]
    [string]$Preset = "Default",

    [Parameter()]
    [ValidateSet("Default", "OahuFlyoverHero", "OahuTopDownMap", "ParticlesHero", "StarfieldHero", "RandomLinesHero")]
    [string]$Presentation = "Default",

    [Parameter()]
    [int]$CaptureWidth = 0,

    [Parameter()]
    [int]$CaptureHeight = 0,

    [Parameter()]
    [string]$CaptureOutput,

    [Parameter()]
    [double]$FixedDelta = 0,

    [Parameter()]
    [int]$CaptureFrame = 0,

    [Parameter()]
    [int]$ExitFrame = 0,

    [Parameter()]
    [switch]$OahuTopDown,

    [Parameter()]
    [switch]$Focus,

    [Parameter()]
    [switch]$NoOverlay,

    [Parameter()]
    [switch]$StayOpen,

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

if ($Preset -ne "Default") {
    $parameters.Preset = $Preset
}

if ($Presentation -ne "Default") {
    $parameters.Presentation = $Presentation
}

if ($CaptureWidth -gt 0) {
    $parameters.CaptureWidth = $CaptureWidth
}

if ($CaptureHeight -gt 0) {
    $parameters.CaptureHeight = $CaptureHeight
}

if (-not [string]::IsNullOrWhiteSpace($CaptureOutput)) {
    $parameters.CaptureOutput = $CaptureOutput
}

if ($FixedDelta -gt 0) {
    $parameters.FixedDelta = $FixedDelta
}

if ($CaptureFrame -gt 0) {
    $parameters.CaptureFrame = $CaptureFrame
}

if ($ExitFrame -gt 0) {
    $parameters.ExitFrame = $ExitFrame
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

if ($StayOpen) {
    $parameters.StayOpen = $true
}

Invoke-PrappyNativeTool "run.ps1" $parameters
