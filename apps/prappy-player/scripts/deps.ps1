#Requires -Version 7.0
# SDL and ImGui sources and libmpv headers are already vendored. Fetch only the pinned DLL.
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
