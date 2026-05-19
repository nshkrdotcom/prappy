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
$presentationDir = Join-Path $captureDir "presentation"
$visualizations = @("RandomLines", "Starfield", "Oahu", "ParticleField")
$presets = @("RandomLinesHero", "StarfieldHero", "OahuFlyover", "OahuCenteredTopDown", "OahuDebugMesh", "ParticlesHero")

$requiredShaderFiles = @(
    "src\shaders\oahu_terrain.vs.sc",
    "src\shaders\oahu_terrain.fs.sc",
    "src\shaders\oahu_terrain_varying.def.sc",
    "src\shaders\oahu_ocean.vs.sc",
    "src\shaders\oahu_ocean.fs.sc",
    "src\shaders\oahu_ocean_varying.def.sc"
)

$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    & python (Join-Path $root "tools\validate_oahu_topology.py")
    if ($LASTEXITCODE -ne 0) {
        throw "Oahu topology validation failed with exit code $LASTEXITCODE."
    }
} else {
    Write-Warning "Python was not found; skipping generated Oahu topology validation."
}

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

function Assert-BmpDimensions {
    param(
        [Parameter(Mandatory=$true)]
        [string]$Path,

        [Parameter(Mandatory=$true)]
        [int]$ExpectedWidth,

        [Parameter(Mandatory=$true)]
        [int]$ExpectedHeight
    )

    $info = Read-BmpInfo $Path
    if ($info.Width -ne $ExpectedWidth -or $info.Height -ne $ExpectedHeight) {
        throw "Expected $ExpectedWidth x $ExpectedHeight capture, got $($info.Width) x $($info.Height): $Path"
    }

    return $info
}

foreach ($shaderFile in $requiredShaderFiles) {
    $shaderPath = Join-Path $root $shaderFile
    if (-not (Test-Path $shaderPath)) {
        throw "Missing required shader source: $shaderFile"
    }
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Config $Config
}

New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
New-Item -ItemType Directory -Force -Path $presentationDir | Out-Null

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

$beforeDiagnostic = Get-ChildItem $captureDir -Filter *.bmp -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

& (Join-Path $PSScriptRoot "run.ps1") `
    -Config $Config `
    -Visualization Oahu `
    -OahuDiagnostic All `
    -Focus `
    -NoOverlay `
    -SmokeTest `
    -ScreenshotSmoke

$afterDiagnostic = Get-ChildItem $captureDir -Filter *.bmp -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $afterDiagnostic) {
    throw "No diagnostic capture produced for Oahu"
}

if (
    $beforeDiagnostic -and
    $afterDiagnostic.FullName -eq $beforeDiagnostic.FullName -and
    $afterDiagnostic.LastWriteTime -le $beforeDiagnostic.LastWriteTime
) {
    throw "Diagnostic capture was not refreshed for Oahu"
}

$diagnosticInfo = Read-BmpInfo $afterDiagnostic.FullName
Write-Host ("[OK] OahuDiagnostic: {0}x{1} {2}bpp -> {3}" -f $diagnosticInfo.Width, $diagnosticInfo.Height, $diagnosticInfo.BitsPerPixel, $afterDiagnostic.FullName) -ForegroundColor Green

foreach ($preset in $presets) {
    & (Join-Path $PSScriptRoot "run.ps1") -Config $Config -Preset $preset -NoOverlay -SmokeTest
    Write-Host "[OK] Preset smoke: $preset" -ForegroundColor Green
}

$presentationCaptures = @(
    @{
        Presentation = "OahuFlyoverHero"
        Width = 1280
        Height = 720
    },
    @{
        Presentation = "OahuTopDownMap"
        Width = 1024
        Height = 1024
    }
)

foreach ($capture in $presentationCaptures) {
    $presentation = $capture.Presentation
    $output = Join-Path $presentationDir "$presentation.bmp"
    & (Join-Path $PSScriptRoot "present.ps1") `
        -Config $Config `
        -Presentation $presentation `
        -Width $capture.Width `
        -Height $capture.Height `
        -Output $output `
        -SkipBuild

    $info = Assert-BmpDimensions $output $capture.Width $capture.Height
    Write-Host ("[OK] Presentation {0}: {1}x{2} {3}bpp -> {4}" -f $presentation, $info.Width, $info.Height, $info.BitsPerPixel, $output) -ForegroundColor Green
}

& (Join-Path $PSScriptRoot "run.ps1") -Config $Config -Renderer D3D11 -Visualization ParticleField -SmokeTest
Write-Host "[OK] Renderer override: D3D11 ParticleField smoke" -ForegroundColor Green
