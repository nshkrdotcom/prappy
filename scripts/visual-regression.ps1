#Requires -Version 7.0
param(
    [Parameter()]
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",

    [Parameter()]
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$configName = $Config.ToLowerInvariant()
$buildDir = Join-Path $root "build\windows-msvc-$configName"
$captureDir = Join-Path $buildDir "captures"
$visualizations = @("RandomLines", "Starfield", "Oahu", "ParticleField")

function Read-BmpInfo {
    param(
        [Parameter(Mandatory=$true)]
        [string]$Path
    )

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 54) {
        throw "BMP is too small: $Path"
    }

    if ($bytes[0] -ne 0x42 -or $bytes[1] -ne 0x4d) {
        throw "Not a BMP file: $Path"
    }

    $pixelOffset = [BitConverter]::ToInt32($bytes, 10)
    $width = [BitConverter]::ToInt32($bytes, 18)
    $heightRaw = [BitConverter]::ToInt32($bytes, 22)
    $bitsPerPixel = [BitConverter]::ToInt16($bytes, 28)
    $height = [Math]::Abs($heightRaw)

    if ($width -lt 320 -or $height -lt 240) {
        throw "Capture dimensions are unexpectedly small: $width x $height ($Path)"
    }

    if ($bitsPerPixel -ne 32) {
        throw "Expected 32-bit BMP, got $bitsPerPixel bits: $Path"
    }

    $sampleStride = [Math]::Max([int][Math]::Floor(($bytes.Length - $pixelOffset) / 4096), 4)
    $firstB = $bytes[$pixelOffset]
    $firstG = $bytes[$pixelOffset + 1]
    $firstR = $bytes[$pixelOffset + 2]
    $differentPixels = 0

    for ($i = $pixelOffset; $i + 2 -lt $bytes.Length; $i += $sampleStride) {
        if ($bytes[$i] -ne $firstB -or $bytes[$i + 1] -ne $firstG -or $bytes[$i + 2] -ne $firstR) {
            $differentPixels++
            if ($differentPixels -ge 8) {
                break
            }
        }
    }

    if ($differentPixels -lt 8) {
        throw "Capture appears blank or nearly uniform: $Path"
    }

    [pscustomobject]@{
        Width = $width
        Height = $height
        BitsPerPixel = $bitsPerPixel
    }
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Config $Config
}

New-Item -ItemType Directory -Force -Path $captureDir | Out-Null

foreach ($visualization in $visualizations) {
    $before = Get-ChildItem $captureDir -Filter *.bmp -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    & (Join-Path $PSScriptRoot "run.ps1") -Config $Config -Visualization $visualization -SmokeTest -ScreenshotSmoke

    $after = Get-ChildItem $captureDir -Filter *.bmp -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $after) {
        throw "No capture produced for $visualization"
    }

    if ($before -and $after.FullName -eq $before.FullName -and $after.LastWriteTime -le $before.LastWriteTime) {
        throw "Capture was not refreshed for $visualization"
    }

    $info = Read-BmpInfo $after.FullName
    Write-Host ("[OK] {0}: {1}x{2} {3}bpp -> {4}" -f $visualization, $info.Width, $info.Height, $info.BitsPerPixel, $after.FullName) -ForegroundColor Green
}

& (Join-Path $PSScriptRoot "run.ps1") -Config $Config -Renderer D3D11 -Visualization ParticleField -SmokeTest
Write-Host "[OK] Renderer override: D3D11 ParticleField smoke" -ForegroundColor Green
