#Requires -Version 7.0
param(
    [Parameter()]
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_tooling.ps1"

Invoke-PrappyNativeTool "configure.ps1" @{ Config = $Config }
