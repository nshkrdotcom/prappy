#Requires -Version 7.0
# SDL, ImGui and libmpv headers are vendored. Fetch pinned mpv and FreeType artifacts.
$ErrorActionPreference='Stop'
$mpv = Join-Path $PSScriptRoot '..\third_party\mpv'
$url = 'https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261021/mpv-dev-x86_64-20261021-git-e471496c56.7z'
$expected = 'EFC719EFF3DC5954B4A5EFC800632808BCEC01CC9034940DD14E5EB7FE9EA633'
$stage = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
New-Item -ItemType Directory $stage | Out-Null
$archive = Join-Path $stage 'mpv.7z'
Invoke-WebRequest $url -OutFile $archive
if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne $expected) { throw 'libmpv archive checksum mismatch. Stop.' }
& tar -xf $archive -C $stage libmpv-2.dll
if ($LASTEXITCODE -ne 0) { throw 'libmpv extraction failed. Stop.' }
Copy-Item -LiteralPath "$stage\libmpv-2.dll" -Destination $mpv
Write-Host "Pinned libmpv runtime ready at $mpv"
$fontSource = Join-Path $PSScriptRoot '../third_party/freetype'
$fontArchive = Join-Path $stage 'freetype.tar.gz'
Invoke-WebRequest 'https://codeload.github.com/freetype/freetype/tar.gz/refs/tags/VER-2-13-3' -OutFile $fontArchive
if ((Get-FileHash $fontArchive -Algorithm SHA256).Hash -ne 'BC5C898E4756D373E0D991BAB053036C5EB2AA7C0D5C67E8662DDC6DA40C4103') { throw 'FreeType archive checksum mismatch.' }
New-Item -ItemType Directory -Force $fontSource | Out-Null
& tar -xf $fontArchive -C $fontSource --strip-components=1
if ($LASTEXITCODE -ne 0) { throw 'FreeType extraction failed.' }
Write-Host "Pinned FreeType 2.13.3 source ready at $fontSource"
