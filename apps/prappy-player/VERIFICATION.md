# Windows verification

## Unicode, idle rendering and transport alignment, 2026-10-26

Release, Release with MSVC static analysis, Debug and Debug with AddressSanitizer
build with warnings treated as errors. The health matrix exercises playback,
state/storage/analysis, feature UI and native window behavior in Release, Debug
and ASan; Debug instances also produce clean CRT leak reports. The normal Release
executable was rebuilt after isolated suffix-build verification.

New checks cover CJK, Arabic, accented text, color-emoji sequences and complete
cluster highlighting, scaled text, idle/caret deadlines, and elapsed-time gain
and meter release. A multilingual filename is rendered and found by a Unicode
commit through the SDL backend. SDL preedit events and composition Enter/Escape/
Space preserve paused playback. A settled paused window presents no further
frames across a two-second observation. Compact, wide and restored layouts check
that playback status and meter occupy the same centered row. DirectWrite text
uses the FreeType capital height and baseline to match surrounding control text.
Both README screenshots were recaptured from the actual library after scan and
waveform reconciliation, using isolated copied state and silent playback.

These automated composition tests inject SDL events. This desktop has only the
English keyboard layout; native Japanese/Chinese/Korean conversion engines and
candidate popups still require a manual check on a desktop with those IMEs.
The pinned SDL backend owns native text-input start/stop and caret positioning;
Windows owns composition and candidate UI.

Eight-second CPU samples on this desktop, expressed as percent of one core:

| Mode | Fixed 30fps loop | Event loop, same test polling | Production wait behavior |
|---|---:|---:|---:|
| Paused compact | 24.6% | 2.5% | 0.0% |
| Paused 900×760 | 80.0% | 2.5% | 1.0% |
| Minimized | 0.2% | 4.7% | 0.0% |

The explicit test harness polls command files every 16ms; normal sessions do
not. `scripts/measure-idle.ps1 -Variant Release -Label after -NativeWait` reproduces
the production-wait measurement after `scripts/test-dedup.ps1` creates its audio
fixtures. Startup/resize settling is excluded; numbers are finite samples,
not universal CPU guarantees. Playback remains animated around 30fps. Working
set rose from roughly 39–45MB to 142–150MB with loaded fallback fonts and shaped
text; rasterized filename tiles have a 16MiB cache budget, with eviction deferred
until the draw list using a texture has been presented.

## Preserve list scroll during playback, 2026-10-26

Playback generation changes previously dirtied the visible rows and implicitly
requested a scroll reset, jumping the library to the top after a song loaded.
Row invalidation and explicit scroll reset are now separate. Playback updates
refresh History as needed without scrolling either view; library selection only
updates its highlight. Search, sorting and view switches still reset scroll.

The feature UI regression adds 120 distinct WAV fixtures across six directories,
scrolls below the top and clicks a visible song in each sort mode. It checks the
actual child scroll offset after playback commits and after the following frame
(when ImGui applies queued scroll targets). The test failed before the fix in
Filename mode, then passed for Filename, Directory groups and File type groups.

## Shared text baseline, 2026-10-26

Centering each word's full visible bounds caused History's descender to shift
its capitals two pixels above List. Labels now share the font's cap-height
baseline while retaining horizontal ink centering. A screenshot regression
checks actual pixels of the initial H and L, independently of button rectangles.
Before the fix it measured H=59..67 and L=61..69; afterward both are 61..69.
The check covers compact, expanded and 900-pixel-wide layouts.

## Layout polish, 2026-10-26

History and List now share identical 74-by-26-pixel bounds. List uses a drawn
chevron; the label and chevron are centered together. Back/Next use vector
transport icons, and all transport buttons share integer-pixel vertical bounds.
The volume value is separate from the slider, whose thumb cannot cover text.
Status sections use middle dots; an initial scan with no catalog summary has no
leading separator. History timestamps use the same separator style.

The feature UI check reproduced the volume-overlap issue before the fix, then
passed the new non-overlap and matching-bounds checks after it. Checks cover
volume endpoints 0/100, 500/900-pixel widths, all repeat states/tooltips, history
navigation and search. Compact/expanded screenshots were visually inspected.
State checks cover empty and populated status composition. The full health
suite passed with strict Release, Debug, static-analysis and ASan builds, clean
Debug CRT leak reports, and native resize/DPI/input/shutdown checks.

## Persistent catalog, history and repeat, 2026-10-26

The final `verify-health.ps1` run passed with MSVC 19.44: Release, Debug and
Debug-Asan runtime suites plus a clean `/analyze` build. `/W4 /WX /permissive-`
remain enforced. Debug CRT reports passed for playback, state integration,
feature UI and native window checks; no ASan errors were detected.

State integration covers:

- Default 10,000-entry retention, configurable limits, branch-preserving history,
  Back/Forward without duplicate visits, pruning an active old visit and pruning
  while navigation is pending.
- UTF-8, Unicode/UNC paths, LF and Windows CRLF/BOM history, malformed/truncated
  records, bounded lengths, backup rotation/recovery, valid-entry salvage with
  damaged-source preservation, newer-version preservation and failed/locked writes.
- Independent binary catalog format version, checksum/config validation,
  modification-aware verification reuse, forced revalidation, changed/deleted
  files, offline roots/hard links and configured aliases of canonical paths.
- Real mpv playback with rapid requests and stale events, failed-load/decoder-error
  rollback, pause intent during loading, and paused position/repeat restoration.
- Natural EOF in Off, One and All; manual Next with One enabled; one-track All
  looping and empty-library handling.

Feature UI tests use explicit `--window-test` instances and isolated fixtures.
They send acknowledged ImGui input events, waiting for observed hover, press,
activation and state changes. They do not use fixed sleeps to assume readiness
and do not compete for the physical desktop cursor. Tests inspect the actual
history file, all repeat tooltip strings, History opening, row selection and
search filtering. Screenshots of the repeat graphics and history view were
visually inspected. Native resize/DPI/hit regions and clean exit are tested
separately by `verify-window.ps1` through Win32. Test state never uses normal
Release history/config/catalog files.

Additional final Release, Debug and Debug-Asan integration runs passed FLAC,
WAV, MP3, Opus, AAC/M4A and WavPack playback, including muted Windows WASAPI output.
The Debug six-format run also reported no CRT leaks. Generated logs and fixture
state remain under `build/health`, `build/state-tests` and `build/feature-ui`.

Catalog verification reuse assumes unchanged path, size and modification time
means unchanged contents. Shift+Refresh explicitly forces content verification.
Tests do not simulate sudden power loss or prove absence of all defects, and
the prebuilt mpv DLL remains outside ASan/Debug CRT instrumentation coverage.

## Button alignment, 2026-10-26

Button labels now center their visible glyph bounds, including the compact List
button and the Previous, Play/Pause, Next, Refresh/Scanning, Config and Clear
buttons. The volume slider is vertically centered within the transport row.
Standard ImGui button input, focus, hover, disabled state and hit regions remain
in use. The helper handles the player's ASCII control labels and hidden IDs.

Debug, Release and `/analyze` builds passed with warnings treated as errors.
`verify-window.ps1 -Interactive` passed for Debug and Release, including List
expansion/collapse, transport hit regions, resize/DPI behavior and clean shutdown.
Compact and expanded screenshots were visually inspected for label alignment.

## C++ robustness checks, 2026-10-26

MSVC 19.44 built Release, Release-Analyze, Debug and Debug-Asan successfully.
Player compilation uses C++20 and `/W4 /WX /permissive-`. The `/analyze` build
passed without player diagnostics. Debug incremental linking is disabled to
avoid its conflict with the embedded application manifest. ASan instruments
the player, SDL and ImGui; the prebuilt mpv DLL remains uninstrumented.

`pwsh scripts/verify-health.ps1 -SkipBuild` passed against the final builds:

- Two playback/dedup/search regression runs each in Release, Debug and Debug-Asan.
- Each run includes 12 create/load/destroy playback cycles, library refreshes
  and cancellation/join of asynchronous scans.
- Native window, resize, hit-test, DPI and clean shutdown checks in all three
  runtime variants.
- Debug CRT leak reports passed for both playback runs and window shutdown.
- No ASan errors were reported; all tested processes exited successfully.

Additional integration runs using the existing six-format fixtures passed in
Release, Debug and Debug-Asan: FLAC, WAV, MP3, Opus, AAC/M4A and WavPack, with
decode, duration, pause, seek, volume, sorting, refresh, EOF and muted Windows
WASAPI output checks. The Debug six-format run also reported no CRT leaks.
Local logs and fixtures are under `build/health` and `build/test-media`.

The first Debug regression run exposed a 72-byte SDL allocation at process exit.
The self-test path used SDL timers without shutting SDL down. Shared scoped
shutdown removed that allocation; the same check then passed. COM cleanup and
exception-path releases for command-line arguments and taskbar metadata now
use scoped ownership, and the audio resource owner cannot be copied or reopened.

Windows Application Control blocked launching the final Release-Analyze binary
(Code Integrity event 3077). Compilation/static analysis passed; runtime checks
use the ordinary Release build and the two diagnostic builds. No security policy
was changed. This suite detects exercised memory errors and Debug CRT leaks;
it does not prove absence of leaks inside mpv, Win32 handle leaks, races or
unexercised defects.

## Earlier functional verification

Verified on this Windows workstation with MSVC 19.44, Windows SDK 10.0.26100.0,
CMake 4.4.4 and Ninja 1.13.2. Final Release build succeeded without compiler
warnings from the player. These checks were run against the executable, not only
source inspection.

The resize/title-bar build passed the updated window checks. Windows Application
Control initially blocked the subsequent slim-icon build. For comparison, a
relink using only the previous icon launched and passed the checks. Restoring
the latest icon and relinking also launched and passed the same checks, followed
by the duplicate/playback integration test. The normal player then launched from
`C:\\` and remained responsive, with Smart App Control still On. No framework
code or security settings changed during this comparison. This isolates a
build-specific acceptance problem; it does not establish the reason for the
earlier rejection or guarantee acceptance of future rebuilds.

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
- Space was checked in the live player: one press resumed playback and the next
  paused it. The shortcut and transport button use the same playback action;
  held-key repeats do not toggle it repeatedly.
- Search integration checks cover words in any order, case and Unicode matching,
  either path separator, hidden-copy paths, identity under all sort modes, an
  empty query restoring the list, and a query with no matches. Filtering leaves
  the underlying library intact. Native window and playback checks also pass.
- Launch from outside the source/output working directory.
- Updated window checks at 144 DPI (150% scale): minimum content width is 500
  physical pixels; width and height resize freely, native maximize fills the
  screen width, and restore preserves the previous width. Native left/right,
  top/bottom and diagonal corner hit regions passed, along with whitespace
  dragging and normal input for buttons and sliders. The dark native title bar
  supplies standard minimize/maximize/close controls. Snap Layouts use the native
  maximize button; the flyout itself has not been exercised automatically.
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

The decorative angled strip and its row were removed; collapsed height is now
250px. Native dragging uses unused space throughout the window. Per-frame widget
rectangles exclude buttons, sliders, song rows, group headers and scrollbars;
active controls and open menus retain mouse handling. Native hit tests verify
empty header/transport areas drag while header buttons, Play, seek and volume
remain client input. Top/bottom edges still resize height.

NVIDIA overlay mitigation: the prior GPU-rendered process loaded `d3d11.dll`,
`DXGI.DLL` and NVIDIA's `nvspcap64.dll`. The player now explicitly uses SDL's
software renderer with framebuffer acceleration disabled. After rebuilding and
launching through the existing pinned shortcut, none of those three modules
were loaded. The UI screenshot and native window/control hit tests passed.
NVIDIA's global settings were not changed.

Limitations: audible output was not listened to; hardware output initialization
was checked while muted. Scraper metadata is not part of the player's scope.
Dragging between physical monitors with different DPIs and a manual visual
taskbar/Alt-Tab inspection were not performed. Native styles, icons and constraints
were checked through Win32 APIs. The old visualizer's source was moved intact;
its shared-tooling resolution and doctor passed, but it was not rebuilt here.

## Module refactor and audio analysis — 2026-10-26

`pwsh scripts/verify-health.ps1` passed for the completed refactor and analysis
implementation. Release, Release-Analyze, Debug and Debug-Asan compiled with
warnings treated as errors; MSVC static analysis passed. Release, Debug and
Debug-Asan each passed two playback/dedup/search regression runs (72 lifecycle,
refresh and scan-cancellation cycles across the matrix), state/recovery tests,
acknowledged UI tests and native window checks. All five Debug CRT reports
reported no leaks; ASan reported no memory errors. The mpv DLL retains the
separate instrumentation limitations described in the README.

New checks cover immutable history snapshots during position changes, append
and pruning; decoded audio format details; a stereo 1 kHz reference signal,
6.0206 dB gain difference, loudness gating, silence/short signals, nonfinite
samples, peak extrema and pyramid tails. Cache tests cover source path/size/time
invalidation, checksum damage, truncation, unknown versions, inconsistent peak
levels, fresh analysis, reuse, forced recomputation and corrupt-cache rebuild.
Decoder tests cover missing/invalid media, cancellation before and during
decoding, and worker shutdown. State test directories include a monotonic-time
suffix to avoid stale fixtures when Windows reuses process IDs.

The UI test restores a history path that is a hidden identical copy, observes
its codec/rate/channel/loudness tooltip, and checks that two independent analysis
cache files are written. It establishes ImGui focus before the initial text
hover. Existing repeat, history branching, list-scroll, geometry, native input
and clean-close checks continue to pass. The format tooltip screenshot was
visually inspected; waveform/histogram rendering remains deferred.

Additional existing representative fixtures passed playback checks for WAV,
FLAC, MP3, Opus, M4A and WavPack. A separate production-worker run generated
valid analysis caches, finite loudness and nine peak levels for all six. WAV,
FLAC, Opus and WavPack measured 3.000 seconds / -21.76 LUFS; MP3 measured
3.000 seconds / -22.20 LUFS; M4A measured 2.977 seconds / -21.76 LUFS. These
values describe the decoded 48 kHz stereo render, not native multichannel
loudness or true peak. Logs are in `build/final-health.log`,
`build/formats-selftest.log` and `build/formats-analysis.log`.

## Browsing, queue and playback features — 2026-10-26

Final verification passed with:

```powershell
pwsh scripts/verify-health.ps1 -VariantSuffix Features -Repetitions 1
```

Release-Features, Release-Analyze-Features, Debug-Features and Debug-Asan-Features
compiled with warnings treated as errors. Static analysis passed. Release,
Debug and ASan passed playback/search/dedup regressions, state and analysis
tests, the expanded UI suite and native window checks. This final matrix
included 36 playback lifecycle/refresh/scan cancellation cycles. Debug CRT leak
checks passed, including both feature-test instances across the restart check;
ASan reported no memory errors. Logs: `build/features-final.log`.

New model and playback regressions cover:

- WSL hostname/alias/extended UNC conversion, Unicode, spaces and rejection of
  other Windows shares.
- Planned slots in the existing navigation trail: next/append ordering, saved
  queue, branching, idle/rapid Next, successful consumption, failed-load rollback,
  in-flight removal without ghost visits and retention by successful-play time.
- Unplayed/most/last views over retained visits, duplicate-copy statistics,
  filename/path/alias ranking, duration/type/offline filters and unknown-duration
  sorting.
- Normalization target, boost cap, sample-peak headroom, missing measurements,
  independent mpv gain/volume, live decoded PCM metering and pause reset.
- Folder playback/repeat, empty folders and a folder containing only a hidden
  identical copy, with its copy path preserved during progression.
- Saved topmost/strip/normalization/folder preferences.

The UI suite uses acknowledged input events and observed state. It checks
waveform mouse seeking while paused, keyboard Home, context path actions,
queue creation/consumption, all history-derived views, visible gold token
highlighting in a captured row, duration sorting and filters, directory repeat,
native topmost style, applied normalization gain, strip layout, and combined
paused queue/preferences restoration. It then checks both strip and topmost
keyboard shortcuts, including native style removal. ImGui layout/input errors
fail the suite. Token highlighting and strip/waveform screenshots were visually
inspected. Clipboard contents and Explorer windows were left untouched by
automation; path conversion and menu availability are covered without those
desktop side effects.

WAV, FLAC, MP3, Opus, M4A and WavPack fixtures also passed the playback
integration check with the new stereo signal path (`build/features-formats.log`).
The running Release player was preserved; the suffix builds isolate verification
from its executable and state. Catalog format and analysis-cache schema remain
unchanged. The waveform reads selected-track peaks through the existing single
analysis worker, and metering uses the playback decoder rather than another
source read.
