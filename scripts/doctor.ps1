#Requires -Version 7.0
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_tooling.ps1"

Invoke-PrappyNativeTool "doctor.ps1"

