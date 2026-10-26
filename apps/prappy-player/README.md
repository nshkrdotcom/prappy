# Prappy Player

Compact Windows x64 music player: SDL3, Dear ImGui and audio-only libmpv.
Defaults to **500 physical pixels** of content width (also the minimum), with
18px text and adjustable width and height. Starts with
the list collapsed; playback controls always stay at the bottom.
The UI uses SDL's software renderer with a native Windows framebuffer, avoiding
a Direct3D/OpenGL game graphics device that can trigger GPU overlay notices.

## Build and run

PowerShell 7, MSVC 2022 C++ Build Tools / Windows SDK, CMake 3.25+ and Ninja.
The shared toolchain setup is in `dotfiles_private/windows/bootstrap_windows`.
Set `DOTFILES_PRIVATE` if that checkout is not beside this repository or under
`$HOME/projects`. No bgfx, shader compiler, submodules or package manager needed.

```powershell
cd C:\Users\Home\projects\prappy\apps\prappy-player
pwsh scripts/deps.ps1          # first checkout: download the pinned libmpv DLL
pwsh scripts/build.ps1 -Run    # Release; add -Config Debug for a debug build
```

Executable: `build/Release/PrappyPlayer.exe`. Launch it directly without a
console, including from Explorer. To relocate it, copy the executable,
`libmpv-2.dll`, `licenses/` and your `player.ini` together. The MSVC runtime and
SDL are statically linked. Build output and the downloaded DLL are gitignored.

## Music roots

On first launch, the player creates `player.ini` **beside the executable**, with
your Windows Music folder as the default root. Expand **List**, then **Config**
to edit it. UTF-8, one `key=value` per line:

```ini
# Examples: replace these paths with accessible Windows folders/shares.
directory=D:\Music
directory=\\ubuntu-host\music
extensions=flac,wav,mp3,opus,m4a,wv
```

Repeat `directory` for more recursive roots. Relative paths resolve against the
configuration file's directory, never the shell's working directory. Extensions
are case-insensitive, with or without leading dots. Unknown keys and malformed
entries show an error and retain the previous list. Missing roots show a warning;
valid roots still load. Overlapping roots are deduplicated. Directory junctions
are not traversed. No watchers, database, automatic rescans or media writes.

Byte-identical files also collapse into a single song entry, including renamed
copies and hard links. Hover the entry to see all its paths. The status line
reports how many copies were hidden. Same filenames with different bytes stay
separate; different encodings of the same performance also stay separate.
Hard links are recognized by file identity. Other equal-size candidates with
matching small samples receive a full-file SHA-256 check before hiding anything.
Each candidate is hashed at most once per scan. This happens during the
startup scan and explicit Refresh; no files are deleted and no index is stored.
Full-content verification can make those scans substantially slower over WSL,
since candidate audio must be read across the Windows/Linux filesystem boundary.
The window opens immediately and shows scan/duplicate-check progress. A single
temporary worker runs only at startup or when Refresh is clicked; controls stay
responsive and the previous list remains usable until the new list is ready.
Closing cancels the scan. Refresh is disabled while a scan is already running.
Reopening the executable or its pinned taskbar shortcut restores the existing
player instead of launching another copy.

For **local WSL**, Windows already exposes the Linux filesystem: no network
share, SSH, copying or mapped drive is needed. This workstation's running distro
is `ubuntu26-dev`; its configured audio roots are:

```ini
directory=\\wsl.localhost\ubuntu26-dev\home\home\p\g\n\scraper\library
directory=\\wsl.localhost\ubuntu26-dev\home\home\.local\share\audioharvest\phase-02-library
extensions=flac,wav,mp3,opus,m4a,wv
```

Find your distro name with `wsl --list --verbose`. Use that name after
`\\wsl.localhost\`, followed by the Linux absolute path with backslashes.
The distro must be available. Paste the entries into the executable's
`player.ini` and click Refresh. The player reads audio in place and does not
operate the scraper, change its recordings or interfere with collection jobs.

## Controls

- **List** expands/collapses the library. Choose filename order, directory groups
  or file type groups. Rows show each track's relative path, including its
  filename. Paths under `scraper` start after that directory, for example
  `library\beat-suite\render\mix.flac`. Other roots use their folder name as
  the prefix. Hover for the same path and any identical-copy locations;
  scroll horizontally to read longer rows.
- Previous, Play/Pause, Next, seek and volume are always at the bottom.
  Automatic progression stops after the final track; explicit Next wraps.
  Playback errors stop progression and appear in the status line.
- Sorting changes next/previous order without changing the playing track.
  Refresh reloads config and scans once, preserving playback by full path.
  If the playing file disappears from the list, it can finish; Next starts at
  the first remaining track.
- **Refresh** is the single rescan action. Hover over the status for full errors.
- Drag any unused space, including the title and blank list/transport areas.
  Buttons, sliders, song rows, group headers and scrollbars retain normal input.
  Resize any native window edge or corner.
  The dark Windows title bar provides minimize, maximize/restore and close,
  plus the system menu and Windows 11 Snap Layouts when enabled in Windows.
  List toggles preserve your chosen width and leave maximized windows maximized.

The content uses physical pixels; Windows sizes the native frame for each monitor's
DPI. The compact layout was enlarged
from the initial 350px / 12px design at the user's request.

## Verification

The executable includes a headless integration check, using libmpv's null audio
output. Supply a configuration containing at least two nonempty audio files:

```powershell
$p = Start-Process build/Release/PrappyPlayer.exe -WindowStyle Hidden -Wait -PassThru `
  -ArgumentList '--self-test', 'C:\absolute\test.ini', 'C:\absolute\results.log'
$p.ExitCode
```

It checks decoding once per discovered extension, duration, pause, seek, volume,
sort/refresh identity, next/previous, end-of-track progression and error reporting.
Use representative audio files long enough to seek (at least two seconds).
For window checks and screenshots: `pwsh scripts/verify-window.ps1`. This launches
a temporary player instance, checks native window behavior, then cancels its scan
and closes it. Add `-Interactive` to click List on an idle desktop; normal
verification does not move the pointer.
`pwsh scripts/test-dedup.ps1` exercises renamed identical copies, overlapping roots,
alternate-copy queue identity and same-name/same-sample files that differ in the
middle; it also runs the playback integration checks on the deduplicated list.

See [THIRD_PARTY.md](THIRD_PARTY.md) for versions, licenses and provenance.
