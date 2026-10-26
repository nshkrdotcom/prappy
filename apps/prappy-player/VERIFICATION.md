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

Limitations: the user's actual Ubuntu scraper library is not accessible from
this workstation, so its recordings and metadata were not tested. Audible output
was not listened to; hardware output initialization was checked while muted.
Dragging between physical monitors with different DPIs and a manual visual
taskbar/Alt-Tab inspection were not performed. Native styles, icons and constraints
were checked through Win32 APIs. The old visualizer's source was moved intact;
its shared-tooling resolution and doctor passed, but it was not rebuilt here.
