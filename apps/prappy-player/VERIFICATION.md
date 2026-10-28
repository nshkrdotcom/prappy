# Windows verification

## Directory scopes and resizable waveform, 2026-10-26

Directory rules use case-insensitive, separator-aware subtree matching, including
extended UNC paths. Multiple includes form a union; exclusions take precedence.
Duplicate aliases are matched independently, and a permitted alias supplies the
visible row and playback path when the canonical file is hidden. Search excludes
hidden alias paths; directory grouping sorts by the permitted directory. The
directory index merges overlapping roots, counts each recording once per parent,
and ranks first-level folders by count after explicit pins. Only fitting priority
chips are drawn. The one-level popup uses cached filtering and a single clipper.

State tests cover subtree boundaries, sibling prefixes, ancestor exclusions,
multiple includes, aliases, unique parent counts, overlapping roots, pin priority,
UNC/case matching, panel resize clamps and persistence. The feature UI checks two
included chips, nested-folder exclusion, pinning, drill-down, an included child
under a hidden ancestor, and clearing temporary includes. Restart restores hidden
folders, pins and preferred panel height while clearing includes.

Mouse dragging increases the waveform height without moving the playback
position or resuming paused audio. Double-click returns to the 202px transport
height and 26px waveform. The custom height is restored after restart and is
temporarily clamped in a compact window. Settings use checked asynchronous state
writes, with a short debounce while dragging. `player-ui.txt` has format 2; no
catalog/analysis cache invalidation or legacy preference parser is introduced.

Native SDL double-click counts trigger the reset independently of rendering
speed. The reset consumes that click and is applied at the old grip before the
list reflows, preventing a click from selecting a track underneath it. Tests
assert unchanged playback position, paused state and visit count. Small-window
checks verify that browsing controls do not overlap the transport and that List
restores a usable browser size without losing selected directory scopes.

The UI reader retains only coherent snapshots from the same process across
atomic file replacement, and restores its own test window if it is minimized.
Audio information also refreshes on decoder reconfiguration and retries missing
parameters after FILE_LOADED; the state check waits for asynchronous metadata
instead of treating file load as proof that decoder parameters are ready.

## Search/group/filter interactions, 2026-10-26

The UI regression now uses real WAV and big-endian Sun AU files across 120
folders. Combined checks cover directory and filename tokens, case-insensitive
matches, empty results, changing sort modes during search, selecting a result,
history/library switches, Unplayed/Most played/Last played, extension,
availability, duration bounds and measured-duration filters. A matching AU file
is renamed and restored during an active query and extension filter; Refresh
changes the result count from ten to nine and back without losing either input.

Search intentionally presents a flat ranked list across all folders and file
types, including collapsed groups. Clearing it restores the selected grouping
and its collapsed state. Collapse/expand is checked after searches, filters and
refreshes using the actual header positions, independent of transient scrollbar
changes. Resetting filters preserves the query. The playback self-test also
checks WAV and AU decoding, seeking, duration, volume, sorting, refresh and EOF
progression on this mixed-format fixture. These checks require no player behavior
changes; the additional coverage is retained in `scripts/verify-features.ps1`.

## Directory-group scrolling, 2026-10-26

The old grouped view created a separate auto-measuring clipper for every group.
Each clipper submitted its first track, including offscreen groups. Thousands of
offscreen text layouts repeatedly evicted the visible filename cache entries.
Directory and file-type views now cache a flattened sequence of group headers
and tracks, with one fixed-height viewport clipper. Group membership is rebuilt
only after catalog/filter changes; collapsed rows are rebuilt on a toggle.
Headers use the track row height, and collapsed state is kept independently for
directory and file-type sorting.

The model regression covers 10,000 tracks across 5,000 folders, collapse,
switching sort modes and changed visible indices. The UI fixture now spans 120
folders and asserts that drawing remains bounded by viewport height, checks
collapse/expand scroll extents, and retains the existing selection/scroll tests
in all three sort modes. The test reader retries snapshot-open races instead of
occasionally returning a null observation during continuous rendering.
Native window verification also waits for restore animation/resize completion
before asserting the restored width, instead of sampling a transition frame.

On the real library of approximately 6,000 tracks, four-second continuous-frame
samples with isolated copied state showed directory rendering improve from
1.2fps to 28.7fps. Filename mode measured 26.4fps before and 28.9fps after.
The corrected directory frame submitted nine visible tracks. Scanning and lazy
analysis remained active, so these are observed desktop results, not controlled
CPU microbenchmarks. The normal Release player/state was preserved during the
comparison; all test instances close and cancel their work afterward.

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

## Responsive folder row - 2026-10-27

`./scripts/verify-health.ps1 -VariantSuffix Unicode -Repetitions 1` passed
strict Release, static-analysis, Debug and ASan builds, playback/state suites,
feature UI and native-window checks. Debug CRT leak reports passed; ASan reported
no memory errors. Full log: `build/row-health.log`.

The pure compact-row tests cover every width from 0 through 2,000 pixels with
10,000 candidates, checking bounded widths, reserved overflow and measurement
of only the visible prefix plus one candidate. Directory tests cover active-first
ordering, deduplication and missing active folders. DirectWrite tests verify
native ellipsis with CJK, emoji and combining marks without missing glyphs.

UI checks cover the 500px minimum and a wide window, long Unicode labels,
multiple includes, active exclusions, overflow removal/reordering, returning to
tree browsing and saved filter/panel state. Existing grouped scrolling, search,
history, transport alignment, IME and idle-rendering checks also passed.
Narrow/active rows and the overflow picker were visually inspected. Verification
used isolated suffix builds and preserved the running Release player and state.
Both README screenshots were refreshed from the isolated real-library run and visually inspected (`build/row-shots.log`).

## Responsive waveform and seeking - 2026-10-27

The waveform now rasterizes a peak envelope once per source/horizontal resolution
into a bounded 128px-high texture (at most 4096px wide, 2 MiB). It draws two image
quads and a pixel-aligned marker, at most 12 vertices, rather than regenerating
per-column anti-aliased stereo lines. Changing height reuses the raster. Peak
normalization scans the selected level rather than only its first coarse bin.

Input is processed before drawing the marker. Seek commands use mpv's async API,
with one command awaiting a reply and one replaceable latest target. A stationary
held mouse sends no repeated commands. Position, duration, pause and meter use
observed mpv properties, removing steady-state synchronous reads. The UI owns its
texture, consumes worker-produced immutable peaks and never decodes source files;
mpv retains decoder ownership. The marker interpolates playback updates, retains
an exact paused-click preview, and input bypasses the animation frame deadline.
Playback animation targets 60fps; paused idle rendering remains event driven.

Release and Debug state suites passed, including 10,001 coalesced seek targets,
an independent decoder-position check, unknown-duration restore and smooth/paused
clock behavior. The Release submission sample took 0.078 ms; this measures command
submission, not time until sound resumes. Static analysis passed. The normal
Release executable was rebuilt. Six-format playback/seek checks passed for WAV,
FLAC, MP3, Opus, M4A and WavPack. Logs: `build/seek-release-state.log`,
`build/seek-debug-state.log`, `build/seek-analyze.log`, `build/seek-formats.log`.

Release UI checks passed immediate-marker geometry, stationary-click coalescing,
height-independent cache reuse, transport resize/reset/persistence, keyboard seek,
grouped scrolling, filters/search/history, IME and paused idle rendering. The
expanded waveform was visually inspected. Faster input exposed a test that sampled
the previous child-layout scroll range; the test now advances layout before taking
its baseline. Log: `build/seek-ui.log`.

The ASan build compiled. An earlier iteration passed ASan state and playback
checks, but Windows Application Control blocked the final ASan executable at
launch with an Enterprise signing-level policy error (Code Integrity event 3077).
The policy was left unchanged; final ASan runtime verification is unavailable.

Final regular Debug UI and native Release window checks passed. Debug state and
UI reports both say `PASS no CRT leaks`. Logs: `build/seek-debug-ui.log`,
`build/seek-debug-ui-leaks.log`, `build/seek-debug-leaks.log` and
`build/seek-window.log`.
Both README screenshots were refreshed from an isolated real-library snapshot using the final Release executable and visually inspected (`build/seek-shots.log`).

## Stable maximized waveform - 2026-10-27

The previous textured-triangle rendering reproduced a diagonal cutoff in the
SDL software renderer at 1920px width. Its moving partial UVs also changed the
sampling region as playback advanced. Waveform drawing now uses native SDL
rectangular texture copies with a fixed destination and source. Played/unplayed
colors use disjoint clips, avoiding resampling and translucent overdraw at the
split. Renderer clip state and texture color modulation are restored afterwards.
The cached raster and asynchronous seek path remain intact.

A new offscreen test renders through the production ImGui/SDL software backend
at 500x26, 1920x720, 3840x1600 and 1920x720 with 2x renderer/display scaling. It
compares tint-normalized pixels at three marker positions, checks both lanes
reach the right-hand side, and checks no raster rebuild or extra geometry is
introduced. The old committed renderer fails this test at 1920x720
(`build/wavefix-baseline.log`); Release and Debug pass with the fix. The Debug CRT
leak report passes. Logs: `build/wavefix-release-state.log`,
`build/wavefix-debug-state.log`, `build/wavefix-debug-leaks.log` and
`build/wavefix-analyze.log` (strict static-analysis build).

The final native UI run also passed maximize/restore, cached-height resizing,
seeking, saved state and paused idle checks (`build/wavefix-ui-maximized.log`).
Its maximized screenshot was visually inspected: both stereo lanes span the
full width without diagonal truncation. The normal Release executable was rebuilt.

## GPU rendering and 4K responsiveness - 2026-10-27

SDL Direct3D 11 now owns drawing and presentation, with DXGI maximum frame
latency set to one and the existing event scheduler pacing animation. Input
redraws immediately, paused idle rendering remains event driven, and SDL/ImGui
resources stay on the window thread. There is no production software fallback;
`PRAPPY_TEST_SOFTWARE` is honored only by explicit isolated test windows.
`Presentation` owns renderer lifetime and per-frame build/draw/Present diagnostics.
Recoverable SDL device-reset events rebuild waveform, text and ImGui font textures
and reapply latency settings. Unrecoverable device loss remains an explicit error.

DirectWrite layouts are now independent of measurement/color/highlight raster
entries. Measuring a label does not populate a second raster entry. Shared
layout effects are applied and cleared during UI-thread rasterization. Layout
and raster caches are bounded to 1,024 entries each, live tile storage to 64 MiB;
evicted textures survive until the draw data using them has been submitted. The 160-row,
three-frame regression failed before implementation and now verifies that warm
visible text is not reshaped on every frame (`build/gpu-red-state.log`,
`build/gpu-text-state.log`).

A repeatable real-library benchmark compared the same Release executable and
text-cache implementation using software versus Direct3D 11. It used a
3840x2130 content area, about 7,186 recordings and 56-78 visible rows. Four-second
sample windows included normal playback, sustained list scrolling and a 500px
expanded waveform. Ten paused seeks per scene checked the marker was updated in
the acknowledged click frame. Warm stationary expanded-waveform samples produced
zero new layouts or raster tiles. Results (`build/ui-performance.json`):

| Scene | Software fps | GPU fps | Software frame p50/p95 ms | GPU frame p50/p95 ms | Software input p50/p95 ms | GPU input p50/p95 ms |
|---|---:|---:|---:|---:|---:|---:|
| Compact | 62.0 | 62.5 | 4.68 / 5.73 | 0.40 / 0.52 | 5.21 / 6.32 | 0.63 / 0.80 |
| 4K library | 3.0 | 60.2 | 302.95 / 410.61 | 1.38 / 2.79 | 293.76 / 330.65 | 1.41 / 1.52 |
| 4K scrolling | 3.4 | 64.0 | 296.54 / 299.43 | 1.40 / 2.16 | 309.90 / 350.34 | 1.49 / 1.66 |
| 4K expanded waveform | 3.0 | 60.7 | 325.55 / 340.67 | 1.28 / 1.81 | 348.36 / 407.07 | 1.45 / 1.74 |

Input timings measure accepted command processing through Present returning;
they do not include physical input delivery, GPU completion or display scanout.
SDL queues software raster work, so the software Present phase includes much of
the actual drawing cost. Results are samples on this machine, not platform-wide
guarantees. The benchmark saves screenshots and cache counters and checks for
known overlay module injection. The final GPU 4K screenshot was visually inspected.

The user-authorized NVIDIA per-app workaround was applied through NVAPI after
exporting profiles to `build/nvidia-profiles-before-20261027-184058.txt`.
Readback in a fresh process confirms `PrappyPlayer.exe` has
`0x809D5F60=0x10000000`. No global overlay/notification settings were changed.
NVIDIA Overlay and nvsphelper64 processes were running during verification;
no nvspcap/NvCamera capture hooks or other checked overlay modules were loaded
in the GPU test processes. The opt-in script refuses shared/predefined profiles,
backs up before writing and requires administrator privileges for the write.
This undocumented driver flag may need reapplying after driver updates.

Strict Release, static-analysis and Debug builds passed. Release and Debug state
suites, six-format playback/seek checks (WAV, FLAC, MP3, Opus, M4A, WavPack),
playback lifecycle checks, the GPU feature UI suite and native window checks passed.
Debug state and both UI lifetimes reported no CRT leaks. Injected renderer-reset
checks verified texture rebuilding and unchanged paused position; this tests the
recovery path, not an actual forced GPU/driver removal. Logs: `build/gpu-analyze.log`,
`build/gpu-release-state.log`, `build/gpu-debug-state.log`, `build/gpu-debug-ui.log`,
`build/gpu-debug-state-leaks.log`, `build/gpu-debug-ui-leaks.log`,
`build/gpu-playback.log`, `build/gpu-formats.log`, `build/gpu-window.log`.

ASan compiled, but Windows Application Control blocked that executable at launch;
the policy was left unchanged. Final independent review caught the renderer-reset
gap, and a focused follow-up review passed after the fix. The normal Release
executable was replaced with the verified suffix-build binary (SHA-256 matched)
while preserving the running player and its state. Restart uses the GPU build.

Both README screenshots were refreshed using the final GPU Release executable and visually inspected (build/gpu-shots.log).

## Relaunch after blocked window cleanup - 2026-10-27

A closed production instance remained alive with no player window. A live stack
walk placed its main thread in NtUserDestroyWindow, through IMM32/MSCTF and
textinputframework, waiting in CoreMessaging. Audio/render resources had already
shut down, but the process still held the single-instance mutex. New launches
saw that mutex and exited without finding a window to restore. The stale closed
process was terminated and the player reopened successfully.

Shutdown now releases the single-instance handle after playback/state/UI/render
cleanup, before SDL_DestroyWindow. A Windows IME cleanup stall can no longer hold
that lock and prevent another player from opening. This addresses relaunch
lockout; it does not claim to repair Windows' internal text-input deadlock.

The new verify-relaunch.ps1 uses an isolated state directory, a unique test mutex
and test-window-only cleanup gate. With the old release order, the new process
exited instead of opening (build/relaunch-red.log). With the fix, both Release and
Debug open a new instance while the old process is still held in window cleanup,
then both exit cleanly. Both Debug lifetimes have no CRT leaks. The health matrix
now includes this regression. Static analysis and native window/input checks
passed. Logs: build/relaunch-release.log, build/relaunch-debug.log,
build/relaunch-debug-leaks.log, build/relaunch-analyze.log,
build/relaunch-window.log. Independent focused review found no additional issues.
The normal Release executable was rebuilt with the fix.

## Saved short-FLAC startup error - 2026-10-27

The reported bottom-bar error occurred during automatic session restore. The
saved current track was a valid 64ms stereo/24-bit/48kHz FLAC with one 3072-frame
audio block. Independent SoX decoding confirmed its samples. The bundled mpv
failed an initial libavformat seek and returned MPV_ERROR_NOTHING_TO_PLAY before
outputting samples; the problem reproduced from both WSL and a local copy, with
metering disabled, and with other tested mpv builds. Ordinary music files decoded.
No dependency versions or user audio files were changed.

For this exact error on FLAC, Audio now retries once through FFmpeg's sequential
file stream, with a per-file 1 MiB packet-cache limit. It retains the application's
playback serial and real source path, acquires the new mpv playlist-entry identity,
and preserves pause intent. The same recovery is used by the background analysis
decoder. A failed retry still reports the real error. Normal successful decoding
and other formats are unaffected. Restore no longer sends a redundant seek to zero.

A paused tiny clip can reach EOF while the null output primes. Playback must not
automatically navigate in that case. Explicit pause/play intent is now separate
from mpv's reported pause property, which can reset as file-local options unwind
at EOF. This preserves normal Repeat/Next behavior as well as paused restore.
An inactive, already-finished clip also records a newly committed play intent.

A public-domain SoX-generated 60-byte FLAC fixture reproduces the error without
shipping user audio. The old code failed the startup test; the fix passes paused
restore, retained visit count, clean playback EOF and all 3072 analysis frames.
Additional red/green checks cover paused EOF with another available track and
committing play intent after EOF. Release and Debug full state/repeat suites,
Debug playback stress, six-format playback, static analysis and Debug CRT leaks
passed. Logs: build/short-restore-red.log, build/short-paused-eof-red.log,
build/short-intent-red.log, build/short-restore-state.log,
build/short-restore-debug-state.log, build/short-restore-debug-leaks.log,
build/short-restore-playback.log, build/short-restore-formats.log,
build/short-restore-analyze.log.

An isolated copy of the actual saved history, using the original WSL clip,
restored the same cursor paused, generated its 3072-frame waveform and reported
an empty playback error. Its screenshot was visually checked: no startup error
at the bottom. Logs: build/saved-startup.log and build/actual-startup-clip-decode.log.
Independent review caught the inactive-intent edge; a focused follow-up passed.
Native text input is now explicitly stopped during window shutdown, after releasing
the instance lock, so active IME teardown is not left to implicit window destruction.
The final GPU UI and close/relaunch suites passed (build/short-restore-ui.log, build/short-restore-relaunch.log). The final Release executable was installed without interrupting the running session.
## Fractional waveform width artifact - 2026-10-27

The previous odd-integer-width fix did not address fractional layout widths.
Captured the actual Direct3D11 player at window widths 800, 801, 1000, 1001,
1200 and 1201 using the restored library track. The waveform's layout width
ended in .143 pixels. Before this fix, its alternating bar/gap sequence had
one repeated column at screenshot x=463, 464, 580, 581, 698 and 698 respectively
(about 58% across). After snapping both horizontal edges to device pixels and
rasterizing that exact span, all six captures have zero repeated columns in
the lane-center scan, excluding the playback marker. Enlarged before/after
crops were also visually inspected. Captures: build/wave-inspect/.

The expanded pixel regression reproduces the failure on the previous committed
renderer. It now checks fractional bounds using both software and Direct3D11
pixel readback. Release and Debug builds and their complete state regression
suites pass, including 32 waveform cases per build.

## Resource ownership audit, 2026-10-27

The asynchronous analysis read buffer now outlives the pending-I/O guard, so
cancellation and completion draining precede buffer destruction on exceptions.
BCrypt algorithm/hash handles, mpv property strings and known-folder allocations
now have scoped owners before any potentially throwing C++ allocations.

Release, MSVC static analysis, Debug and Debug ASan builds passed. The health
matrix passed Release and Debug playback stress, state/DSP/cache tests, feature
UI, native-window and relaunch checks; Debug CRT leak reports were clean.
The ASan runtime run was blocked by Windows Application Control at process
launch, so no ASan runtime pass is claimed. Log: build/ownership-health.log.
The paused-idle check now waits for an observed quiet period (bounded at twenty
seconds), then independently asserts no further frames over two seconds.
## Complete UI session persistence, 2026-10-27

Preferences format 4 adds search, listening/availability/extension/duration
filters, measured-only, included folders, sort/grouping and collapsed groups,
Library/History selection, List expansion, scroll offsets, folder-browser
location/search, normal window geometry and maximization, and strip mode's
previous normal height/maximization. Versions 2 and 3 remain readable. Session
changes use the existing debounced asynchronous writer and synchronous final
flush. Minimized/maximized dimensions do not replace the normal rectangle;
restored geometry is fitted to an available monitor.

Release, Debug and MSVC static analysis builds passed. Release and Debug passed
the complete playback/dedup stress, state/DSP/cache/preferences, feature UI,
native-window and relaunch matrix, with clean Debug CRT leak reports. Preference
model tests cover full-session round trips, invalid values and v2/v3 migration.
Four UI lifetimes exercise included/hidden folder restoration, strip-to-normal
height, search/filter/grouping/List state, exact native rectangle restoration,
History plus collapsed List plus maximization, and restoring the normal size
from maximization. Log: build/persistence-health.log. ASan runtime remains
unavailable because Windows Application Control blocked its executable launch
in the preceding ownership verification.
