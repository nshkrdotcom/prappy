#Requires -Version 7.0
param(
    [Parameter()]
    [switch]$Refresh
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_tooling.ps1"

$parameters = @{}
if ($Refresh) {
    $parameters.Refresh = $true
}

Invoke-PrappyNativeTool "deps.ps1" $parameters
