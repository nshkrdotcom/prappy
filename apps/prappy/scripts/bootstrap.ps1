#Requires -Version 7.0
param(
    [Parameter()]
    [switch]$Force
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_tooling.ps1"

$tooling = Resolve-PrappyNativeTooling
$bootstrapRoot = Resolve-Path (Join-Path $tooling "..")
$installer = Join-Path $bootstrapRoot "Install-NativeCppToolchain.ps1"

& $installer -Force:$Force

