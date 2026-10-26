#Requires -Version 7.0
param(
    [Parameter()]
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",

    [Parameter()]
    [ValidateSet("OahuFlyoverHero", "OahuTopDownMap", "ParticlesHero", "StarfieldHero", "RandomLinesHero")]
    [string]$Presentation = "OahuFlyoverHero",

    [Parameter()]
    [int]$Width = 0,

    [Parameter()]
    [int]$Height = 0,

    [Parameter()]
    [string]$Output,

    [Parameter()]
    [switch]$Live,

    [Parameter()]
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_tooling.ps1"

$parameters = @{
    Config = $Config
    Presentation = $Presentation
    SkipBuild = $SkipBuild
    Live = $Live
}

if ($Width -gt 0) {
    $parameters.Width = $Width
}

if ($Height -gt 0) {
    $parameters.Height = $Height
}

if (-not [string]::IsNullOrWhiteSpace($Output)) {
    $parameters.Output = $Output
}

Invoke-PrappyNativeTool "present.ps1" $parameters
