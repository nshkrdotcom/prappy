#Requires -Version 7.0
$ErrorActionPreference = "Stop"

function Resolve-PrappyProjectRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Resolve-PrappyNativeTooling {
    $candidates = @()

    if ($env:DOTFILES_PRIVATE) {
        $candidates += (Join-Path $env:DOTFILES_PRIVATE "windows\bootstrap_windows\native_cpp")
    }

    $candidates += @(
        (Join-Path $PSScriptRoot "..\..\..\dotfiles_private\windows\bootstrap_windows\native_cpp"),
        (Join-Path $env:USERPROFILE "projects\dotfiles_private\windows\bootstrap_windows\native_cpp")
    )

    foreach ($candidate in $candidates) {
        $resolved = Resolve-Path $candidate -ErrorAction SilentlyContinue
        if ($resolved -and (Test-Path (Join-Path $resolved.Path "NativeCpp.Common.ps1"))) {
            return $resolved.Path
        }
    }

    throw "Unable to locate dotfiles native C++ tooling. Set DOTFILES_PRIVATE to the dotfiles_private repo root."
}

function Invoke-PrappyNativeTool {
    param(
        [Parameter(Mandatory=$true)]
        [string]$ScriptName,

        [Parameter()]
        [hashtable]$ToolParameters = @{}
    )

    $tooling = Resolve-PrappyNativeTooling
    $script = Join-Path $tooling $ScriptName
    $projectRoot = Resolve-PrappyProjectRoot

    if (-not (Test-Path $script)) {
        throw "Missing native tool script: $script"
    }

    & $script -ProjectRoot $projectRoot @ToolParameters
}
