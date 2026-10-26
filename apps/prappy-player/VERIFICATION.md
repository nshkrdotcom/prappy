# Windows verification

Verified on this Windows workstation with MSVC 19.44, Windows SDK 10.0.26100.0,
CMake 4.4.4 and Ninja 1.13.2. Final Release build succeeded without compiler
warnings from the player. These checks were run against the executable, not only
source inspection.

Commands, from this app directory:

```powershell
pwsh scripts/build.ps1
# Launch --self-test with absolute config/log paths, including from C:\ as CWD.
pwsh scripts/verify-window.ps1
```

Playback integration used six generated three-second sine-wave files encoded
with FFmpeg, one each of FLAC, WAV, MP3, Opus, AAC/M4A and WavPack. The FFmpeg
test tool was downloaded into the system temporary directory, not installed or
added to the application. Fixtures, logs and screenshots are under `build/` and
are not committed.

Passed:

- Decode, duration, pause, seek and volume on all six formats (null audio output).
- Current path preserved while sorting by all three orders and refreshing.
- Duplicate/overlapping music roots deduplicated; missing root visibly reported.
- Previous/next and automatic progression on EOF.
- Missing/unsupported playback file yields an error instead of a crash.
- Missing config retains the previous in-memory library and playback identity.
- Actual Windows WASAPI audio output initialized successfully with volume muted.
- Launch from outside the source/output working directory.
- Width remains exactly 500 physical pixels at 144 DPI (150% scale), including
  native resize and attempted maximize. Height resize, list expansion/collapse,
  pin toggle and native top/bottom resize/transport drag hit regions passed.
- Per-monitor-v2 DPI awareness, runtime icon, ordinary taskbar/Alt-Tab window
  style, and Windows GUI executable subsystem (no console) confirmed.
- Temporary window-test instances close after verification; normal app launch
  remains available independently.

Local WSL integration was subsequently verified with `ubuntu26-dev`. The checkout
library and registered `phase-02-library` are accessible directly through
`\\wsl.localhost\ubuntu26-dev\home\home\...`. Windows enumerated 5,328 audio
files across these roots: 2,372 WAV, 1,604 FLAC, 1,159 WavPack, 157 MP3, 23 Opus
and 13 M4A at verification time. One actual recording of each format loaded and
advanced through decoded playback using the deployed libmpv DLL and the WSL UNC
path, with null audio output. No audio was copied, converted or modified.

The original scan canonicalized every file separately, making WSL startup slow.
It now canonicalizes each configured root once and uses lexical paths for entries,
avoiding redundant UNC metadata requests while retaining overlapping-root dedup.
Enumeration uses Windows extended-length paths internally so deep WSL library
paths also load without requiring a machine-wide long-path setting. The live
library changed during collection; a later player scan loaded 5,329 tracks without
a scan warning.

Duplicate inventory: a read-only WSL scan found 5,328 paths, including 496 hard-link
aliases. Full SHA-256 checks of same-size/same-sample candidates identified 1,090
redundant paths in 775 byte-identical groups, leaving 4,238 unique files at that
time. The player recognizes hard links and checks full-file SHA-256 before hiding other duplicates, retains
alternate paths for tooltips/current-track identity, and leaves every file intact.
The dedup regression also verifies equal-size files with identical sampled ends
but different middle bytes remain separate. Library counts can change while the
user's collection continues running.

The live player showed **4,238 tracks / 1,090 identical copies hidden**, matching
the independent inventory. Hard-link identity is checked without reading audio;
other sample-matched candidates are hashed once per scan using Windows SHA-256.
This avoids repeated pairwise file reads, but full-content scanning across WSL
is still substantially slower than the former path-only scan. No persistent
checksum database/cache was added. The bright mint title/volume/drag details
and gold Play/seek accents were verified in the running window.

Pinned-launch regression: the original synchronous startup scan left two player
processes running without windows after taskbar launches. The existing pinned
shortcut already targeted the correct Release executable. The scan now runs in
one cancellable worker after the window is shown, with atomic progress and a
result swapped into the library by the UI thread. Refresh uses the same worker;
there are no timed scans or watchers. A per-session instance lock makes repeated
launches restore the current window instead of starting another scan.

Testing through the actual `Prappy Player.lnk` taskbar shortcut showed the window
in **307 ms**. The window remained responsive while scanning; reopening restored
it from minimized state with exactly one player process. Closing during the scan
cancelled it and exited in **137 ms**. The renamed-copy/hard-link/collision and
playback integration regression passed after this change.

Limitations: audible output was not listened to; hardware output initialization
was checked while muted. Scraper metadata is not part of the player's scope.
Dragging between physical monitors with different DPIs and a manual visual
taskbar/Alt-Tab inspection were not performed. Native styles, icons and constraints
were checked through Win32 APIs. The old visualizer's source was moved intact;
its shared-tooling resolution and doctor passed, but it was not rebuilt here.
