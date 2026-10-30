# Prappy Player

<p align="center">
  <img src="assets/player-banner.jpg" alt="Sculpted mint glass and gold sound ribbons on charcoal" width="100%">
</p>

Compact Windows x64 music player: SDL3, Dear ImGui and audio-only libmpv.
Defaults to **500 physical pixels** of content width (also the minimum), with
18px text and adjustable width and height. Starts with
the list collapsed on first launch; playback controls always stay at the bottom.
Subsequent launches restore search, browsing filters and included/hidden folders,
sort/grouping and collapsed groups, the Library/History tab, List expansion,
scroll position, folder-browser location/search, and window position, size and
maximization. Strip mode also remembers the previous normal window height.
These join volume, normalization, topmost, panel height, pinned folders and
playback/history state in the existing saved settings. Older settings files
remain readable. Window geometry is fitted to an available monitor on restore.
The UI uses SDL's Direct3D 11 renderer for hardware-accelerated drawing at 4K.
Input, ImGui and rendering stay on the window thread, with one queued GPU frame;
audio seeking and library analysis run independently. Paused idle stays event driven.

Filenames use DirectWrite shaping, Windows font fallback and a bundled Noto CJK
font. CJK scripts, supplementary Unicode characters and color emoji work in
library/history rows, the current title and path tooltips. Highlighting follows
shaped clusters, preserving emoji sequences and combining marks. The search box
uses a 32-bit FreeType font atlas; Windows displays IME composition and candidates
at the text cursor. Composition Enter, Escape and Space are reserved for input.

The paused player waits for events and redraws only when needed, including caret
blinks and delayed tooltips. Playback remains animated; worker completions and
audio events wake the UI, including while minimized. Meter decay uses elapsed time; playback normalization never ramps automatically. Text layouts are shared across measurement, colors and
highlights; separate raster caches retain up to 64 MiB of visible text tiles.
Evicted textures are retired after the frame using them is presented. A recovered
graphics-device reset rebuilds text, waveform and font textures without restarting
audio playback.

Seeking queues work asynchronously on mpv's thread and coalesces waiting targets.
The marker follows clicks immediately and interpolates between playback updates;
input redraws immediately while playback animation targets 60fps. The thin stereo
waveform uses ImPlot with cached envelopes bounded by horizontal resolution.
The sampling editor shares the stereo plot primitive and adds exact PCM detail. Decoder position, duration,
pause updates arrive through mpv events. A worker reads Windows audio-session peaks without blocking UI.

NVIDIA's overlay can mistake a GPU-rendered desktop app for a game. The optional
`scripts/set-nvidia-profile.ps1 -Apply` writes the undocumented per-executable
exclusion `0x809D5F60=0x10000000` for `PrappyPlayer.exe`. Run it in an administrator
PowerShell, then restart the player. It exports all profiles to `build/` before
writing, refuses shared/predefined profiles and reads the value back. Omitting
`-Apply` only checks the value. The player never changes driver settings itself;
global overlay and notification settings remain untouched. This workaround is
driver-dependent and may need reapplying after updates.

`scripts/measure-ui.ps1` compares isolated software and Direct3D 11 test windows
at compact and 4K sizes, including scrolling and an expanded waveform. It records
frame phases, text cache activity and accepted-input-to-Present timing in
`build/ui-performance.json`, and checks for known overlay modules. These CPU
timings are not measured display/photon latency. Software rendering is available
only through the explicit test-window comparison, with no production fallback.

## Catalog, history and repeat

The catalog is saved as `player-catalog.bin` beside `player.ini`. A compatible
snapshot is usable immediately while one background refresh checks for changes.
Catalog format version 1 is independent of the executable/product version:
rebuilding or updating the player does not invalidate it. Root/extension changes
require reconciliation. Offline roots retain cached tracks, marked **Offline**,
until they return. The first scan still has to finish before a cache exists.

History is stored separately in `player-history.txt`. The default retains
10,000 successfully loaded visits. To change it, add this to `player.ini`, then
click Refresh (valid range: 1–1,000,000):

```ini
history_limit=10000
```

**History** in the compact header opens a searchable list of visits, newest
at the bottom, with paths and played times. Library and History buttons switch views.
Library selections create separate visits; pause, seeking and Back/Forward traversal
do not. Clicking a history row starts it from the beginning using that existing
visit by default. Set `add_new_listening_history_entry_when_replaying_history_item=true`
in `player.ini` to record history-row replays as new visits. Unavailable paths are marked after a failed attempt;
history remains available even while the catalog is scanning or a root is offline.

The library's view selector offers **Unplayed**, **Most played** and **Last played**.
These use retained successful visits, combine identical-copy paths, and exclude
pending queue entries. Counts are not lifetime statistics: a track can become
Unplayed again after its visits age out of the configured retention limit.

**Folders** provides directory filtering without opening every directory group.
The single row adapts to the available width: active includes and exclusions first,
then pinned folders and top-level folders ranked by recording count. Long names
use an ellipsis; full paths appear on hover. **+N more** opens a scrollable overview
of the priority folders. Click several chips to include their subtrees together;
click an excluded folder to unhide it. Open **Folders** to browse one level at a time, search
that level, include folders, hide whole subtrees, or pin useful subdirectories.
Counts combine identical copies without counting the same recording twice in a
parent folder. Overlapping configured roots share one tree.

Includes are temporary and reset at launch. Hidden directories and pins are
saved in `player-ui.txt`; hidden ancestors take precedence over included children.
**All folders** clears includes while keeping exclusions, **Clear hidden** removes
exclusions, and the row's **Reset** clears both. Search, history views, file-type
and duration filters combine with directory filters. If an identical recording
has an allowed copy outside a hidden folder, the visible row plays that copy.
These are browsing filters; the queue, trail and **Play/Repeat this folder**
continue to determine automatic playback progression.

**Back** returns to the preceding visit. **Next** follows forward history first,
then library order. After A → B → C, Back returns to B and Next returns to C.
Choosing D while at B makes the navigation trail A → B → D; C stays searchable
in the full history. Hover the arrows to see their destination. Failed loads do
not commit navigation or create visits; the previous recording is restored when
possible, with an error displayed.

The graphical repeat button cycles **Off → One → All → Off**. One repeats only
the current track on natural EOF; explicit navigation still works. All wraps the
library sequence after forward history is exhausted. Off stops at the final
library track; explicit Next can still wrap. Failed playback is not retried in a
loop. The enabled icon uses the accent color, with a **1** mark for Repeat One;
its tooltip explains the current state and next click.

Right-click a library/history row or the current track name to **Play next** or
**Add to queue**. These insert pending slots into the existing navigation trail;
they become visits only after a successful load. Play next inserts immediately
after the current visit, ahead of forward history; Add to queue appends after
the forward trail. Repeated Play next actions put the newest selection first.
An explicit new selection branches away from played forward history while
preserving explicitly queued tracks. Failed loads leave the queued slot pending
and restore the previous recording. Rapid Next during a pending queued load
does not skip that uncommitted slot. **More** lists forward/queued tracks and
lets you remove queued slots, including cancelling a pending queued load.
Queue entries are saved with history, are excluded from visit statistics, and
have their own 10,000-track bound within the overall one-million-record limit.

The same context menu offers **Play this folder** and **Repeat this folder**.
The scope is the file's immediate parent directory, excluding subdirectories.
Folders containing only hidden identical copies remain playable using those
copies' paths, without duplicating the library entries.
Fallback Next/EOF progression uses that directory in the selected library order;
Repeat All wraps it. Explicit queued tracks and forward history take priority,
including paths outside the folder. The status line shows the active scope;
**More → Play all folders** clears it. Folder scope and repeat restore on restart.

Restarting restores the selected visit, position, navigation trail and repeat
mode **paused**. Completed recordings restart at zero. State is checkpointed on
track/navigation changes and pause, about every 15 seconds during playback, and
on close. A crash can lose changes since the last checkpoint. Checked temporary
file replacement and backups protect the previous snapshot; errors appear in
the status line. History accepts UTF-8 with LF or Windows CRLF and an optional
BOM. Recovery tries the backup, then valid independent records, preserving a
damaged original. Unsupported newer history formats are preserved without writes.
Snapshots are bounded to 128 MiB, with bounded records and lengths.

## Memory and robustness checks

Player code builds with C++20, `/W4 /WX /permissive-`; warnings fail the build.
Third-party libraries keep their own warning policies.

```powershell
# Build and verify Release, static analysis, Debug leaks and Debug ASan.
pwsh scripts/verify-health.ps1
# Repeat tests against existing diagnostic builds.
pwsh scripts/verify-health.ps1 -SkipBuild
# Build/test beside a running Release player without replacing its executable:
pwsh scripts/verify-health.ps1 -VariantSuffix Features
# Individual diagnostic builds:
pwsh scripts/build.ps1 -Analyze
pwsh scripts/build.ps1 -Config Debug -Asan
```

The health check compiles the static-analysis variant and runs playback/dedup/search
regression tests twice each against Release, Debug and Debug ASan,
including 12 playback lifecycle, refresh and scan cancellation cycles per run,
then verifies catalog/history persistence, recovery, repeat, and native window
behavior. Feature UI tests use explicit test-window input acknowledgements and
observed hover/press/activation/state changes, rather than fixed timing sleeps.
They also check tooltip text and saved files. Logs live in `build/`.
Debug CRT leak reports are checked automatically; for a manual Debug session set
`PRAPPY_LEAK_REPORT` to an absolute log path before launching and close normally.
Without that variable, leak diagnostics go to the debugger. Release omits the
leak reporter. ASan instruments the player, SDL and ImGui, and the build script
copies its required runtime DLL beside the diagnostic executable.

MSVC ASan detects memory misuse, not leaks. CRT reports cover allocations in the
Debug CRT used by this executable; the prebuilt mpv DLL has separate allocation
and instrumentation coverage. These checks cannot prove absence of all leaks,
Win32 handle leaks or concurrency bugs. If memory or handles keep growing after
warm-up during a longer playback/refresh session, investigate with heap snapshots
or a profiler. The automated playback check needs a working Windows audio device;
window checks need an interactive desktop.

## Screenshots

Keep the list tucked away for playback in a small window.

![Compact Prappy Player with playback controls](assets/player-compact.jpg)

Widen the window and open the list to browse tracks by their relative paths.
The playback controls stay at the bottom.

![Prappy Player with a searchable library, waveform and aligned playback controls](assets/player-library.jpg)

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
`PrappyPlayer.ico`, `libmpv-2.dll`, `licenses/`, `player.ini`, `player-ui.txt`, `player-history.txt`
and `player-catalog.bin` together, including their `.bak` files if present. The MSVC runtime and
SDL are statically linked. Build output and the downloaded DLL are gitignored.

## Music roots

On first launch, the player creates `player.ini` **beside the executable**, with
your Windows Music folder as the default root. Expand **List**, then **Config**
to edit it. UTF-8, one `key=value` per line:

```ini
# Examples: replace these paths with accessible Windows folders/shares.
directory=D:\Music
directory=\\ubuntu-host\music
extensions=flac,wav,mp3,opus,m4a,wv,mka,webm,ogg,aac,aiff,wma
history_limit=10000
```

Repeat `directory` for more recursive roots. Relative paths resolve against the
configuration file's directory, never the shell's working directory. Extensions
are case-insensitive, with or without leading dots. Unknown keys and malformed
entries show an error and retain the previous list. Missing roots show a warning;
valid roots still load, and cached entries from unavailable roots remain. Overlapping
roots are deduplicated. Directory junctions are not traversed. No watchers,
database, periodic rescans or media writes.

Byte-identical files also collapse into a single song entry, including renamed
copies and hard links. Hover the entry to see all its paths. The status line
reports how many copies were hidden. Same filenames with different bytes stay
separate; different encodings of the same performance also stay separate.
Hard links are recognized by file identity. Other equal-size candidates with
matching small samples receive a full-file SHA-256 check before hiding anything.
Verification is cached by path, byte length and modification time. An unchanged
catalog reuses its existing grouping after metadata enumeration; changed catalogs
reuse verification for unchanged candidates. Metadata reuse is a heuristic:
same-size edits that deliberately preserve modification times need **Shift+Refresh**
to force content revalidation. Initial or forced full-content verification can
be slower over WSL because audio crosses the Windows/Linux filesystem boundary.
No media is changed or deleted. The window shows scan/duplicate-check progress. A single
temporary scan worker runs only at startup or when Refresh is clicked; controls stay
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
extensions=flac,wav,mp3,opus,m4a,wv,mka,webm,ogg,aac,aiff,wma
```

Find your distro name with `wsl --list --verbose`. Use that name after
`\\wsl.localhost\`, followed by the Linux absolute path with backslashes.
The distro must be available. Paste the entries into the executable's
`player.ini` and click Refresh. The player reads audio in place and does not
operate the scraper, change its recordings or interfere with collection jobs.

## Controls

- **List** expands/collapses the library. Choose filename order, directory groups,
  file type groups, or duration order (unknown durations last). Rows show each track's relative path, including its
  filename. Paths under `scraper` start after that directory, for example
  `library\beat-suite\render\mix.flac`. Other roots use their folder name as
  the prefix. Hover for the same path and any identical-copy locations;
  scroll horizontally to read longer rows.
- **Search** supports quoted phrases: `"nu nu"` requires those words together
  with the space, in both Library and History. Unquoted words match independently.
  Search sits below the expanded list, above playback. Type words from a
  filename or relative path to filter instantly; all words must match, in any
  order, ignoring case. Matching filenames rank above directory and hidden-copy
  matches; matching tokens are highlighted in the displayed row. Searches use
  a flat ranked list even when grouped browsing is selected. Hidden duplicate
  paths are searchable too. The count
  shows matches out of the full library. **Ctrl+F** focuses search; **Escape**
  or **Clear** clears it. Filtering keeps the playing track and playback order.
- Back, Play/Pause, Next, seek, volume and repeat are always available.
  **Space** toggles play/pause when the player has focus.
  With repeat Off, automatic progression stops after the final library track;
  explicit Next wraps after forward history is exhausted.
  Playback errors stop progression and appear in the status line.
- **Filters** offers extension, online/offline availability, minimum/maximum
  duration in seconds, and measured-duration-only filtering. Zero maximum means
  no upper bound. Active duration bounds exclude tracks awaiting analysis.
  Filters and history-derived views affect browsing; folder scope, the trail
  and queue determine playback progression.
- The seekbar draws cached stereo peaks with a gold playhead and mint played
  region. Drag anywhere to seek; keyboard focus supports Left/Right (five seconds),
  Home and End. It stays seekable while waveform analysis is pending. Display
  amplitude is scaled to the track's peak so quiet files remain readable.
  Drag the small divider above the current title upward to enlarge the waveform;
  playback buttons stay in place. Double-click the divider to restore compact
  height. A focused divider also supports Up/Down and Home. The preferred height
  is saved, clamped to fit smaller windows, and restored when there is room.
- A small meter shows Prappy's Windows audio-session sample peaks after volume/normalization,
  with a release decay. It follows output timing; it is not a true-peak or acoustic loudness meter.
- Right-click a track to **Copy full path**, **Copy WSL Linux path**, or
  **Reveal in Explorer**. WSL conversion supports `\\wsl.localhost\distro\...`,
  `\\wsl$\distro\...`, and extended UNC paths, preserving Unicode and spaces.
  The WSL action is disabled for other Windows paths/shares.
- **More** contains **Always on top**, **Slim strip** and **Loudness match to -18 LUFS**.
  Strip mode keeps transport, waveform and meter in 86 content pixels of height;
  use More to return. Ctrl+Shift+M toggles strip mode, and Ctrl+Shift+T toggles
  topmost. These settings, folder scope, waveform height, hidden directories and
  pins and volume use `player-ui.txt` format 3. Existing format 2 settings are
  preserved with a default volume of 70. Volume changes are saved automatically
  and restored on startup, including mute. Catalog format is unchanged;
  disposable audio analysis now has a separate versioned summary registry.
- Sorting changes fallback library progression without reordering history.
  Refresh reloads config and scans once, preserving playback by full path.
  If the playing file disappears from the list, it can finish; Next starts at
  the first remaining track.
- **Refresh** checks roots/settings again; **Shift+Refresh** also revalidates
  duplicate contents and rebuilds audio analysis. Hover over the status for full errors.
- Hover the current track name for source codec, sample rate, channel layout,
  decoder sample format, available source bitrate, and measured duration/loudness.
  Decoder sample format is not a claim about the source's original bit depth.
- Drag any unused space, including the title and blank list/transport areas.
  Buttons, sliders, song rows, group headers and scrollbars retain normal input.
  Resize any native window edge or corner.
  The dark Windows title bar provides minimize, maximize/restore and close,
  plus the system menu and Windows 11 Snap Layouts when enabled in Windows.
  List toggles preserve your chosen width and leave maximized windows maximized.

The content uses physical pixels; Windows sizes the native frame for each monitor's
DPI. The compact layout was enlarged
from the initial 350px / 12px design at the user's request.

## Audio analysis and locked playback gain

Analysis runs independently of listening. A compact, checksummed `summaries.bin`
registry in `player-analysis-v2/` contains path/size/mtime identities, measured
LUFS, sample peaks, tag findings and retry status. It loads on a dedicated IO
thread without reading the whole library's waveform files. Each waveform is a
separate validated blob, read on demand for the selected source. Newly scanned
or changed sources get a background decode. The old disposable
`player-analysis/` cache is **not** imported or deleted; the new namespace is
built once. Recorded audio, history, user preferences, sample workspaces,
drafts and the source catalog are **not** part of this cache.

By default the analysis service uses `hardware_concurrency - 2` decode workers
(clamped to 1–4), with at most two concurrent WSL/remote decodes. A foreground
source can cancel lower-priority work to obtain a decode slot; ready summaries
and selected-waveform IO do not wait in the decoder queue. Each decode owns
its own libmpv handle and bounded cancellation-aware named pipe, producing
stereo peaks, duration and integrated loudness together. Queued next/history
selections and three library-next hints are prioritized before background
work. Canceled jobs and changed identities are fenced from the currently
selected waveform and playback gain. Errors are visible in the current track
tooltip and in **More**, where **Retry selected analysis** explicitly overrides
the usual retry delay. Old caches are left in place until deliberately removed.

The analysis PCM is 48 kHz floating-point **stereo**, with optional mpv
ReplayGain disabled; mandatory codec output gain is present. K-weighting and
the 400 ms / 100 ms gates follow
[ITU-R BS.1770-5, Annex 1](https://www.itu.int/rec/R-REC-BS.1770-5-202311-I/).
Silent tracks and tracks under 400 ms have no integrated LUFS. The measured
peak is a **sample peak**, not an oversampled true peak, and the measured LUFS
is for this stereo render rather than original multichannel content.

**Loudness match to -18 LUFS** is a per-playback decision, applied once on
matching libmpv `FILE_LOADED` while playback remains paused. The player uses
available, valid track ReplayGain/R128 metadata first, then already-persisted
measured loudness, then its configured fallback (0 dB). A newly completed
measurement or waveform **never** changes gain during that playback: seek,
pause/resume, queue changes, editor work and catalog refresh keep the same
value. A new track start, restart, history traversal or repeat-one start may
choose a newer measurement. Explicit user enable/disable changes **do**
recompute the current gain immediately, using only available metadata. The
user's volume slider remains independent. More and the title tooltip show the
applied dB and its origin. A late measurement is marked for the next playback.

Gain is clamped to a maximum +12 dB and a -1 dB **sample-peak** ceiling when
same-domain peak information is trusted; a positive request is capped at
**0 dB when the peak is unknown**, and attenuation can still apply. Fallback
0 dB is not a promise that the unprocessed source is under the ceiling.
The player does not add a limiter/compressor or claim true-peak protection.

Opus `R128_TRACK_GAIN` uses signed Q7.8 and the -23 LUFS reference (hence a
+5 dB reference offset at -18 LUFS). Decoder-applied Opus header gain is not
added again. Track ReplayGain requires its known 89 dB SPL reference; until a
format-specific calibration is established, this application documents an
**assumed alignment of that reference to -18 LUFS**, not an exact standard LUFS
conversion. Conflicting, malformed or unknown-reference tags are rejected.
Apple/iTunes `iTunNORM` Sound Check conversion is **not yet verified or
supported** and safely falls back to measured loudness or the fallback. Tag
behavior requires validation with the locally pinned libmpv and real fixtures.
No media file or tags are modified.

Optional advanced `player.ini` settings (existing normalization enabled flag
still lives in `player-ui.txt`):

```ini
normalization_target_lufs=-18
normalization_unmeasured_fallback_db=0
analysis_worker_count=0
analysis_remote_concurrency_limit=2
analysis_waveform_memory_mib=128
```

The parser rejects nonfinite/out-of-range values. Target supports -36 through
-5 LUFS; fallback -24 through +12 dB; worker override 0 (auto) or 1–4;
remote limit 1–4; waveform memory 16–512 MiB. Editing these settings requires
restarting the player; Refresh does not recreate the decode worker pool. Explicit changes to the normalization checkbox
apply immediately; measurements arriving afterward do not.

The existing Windows output meter is unchanged. It reads the current
process's Windows audio-session sample peaks after application volume/gain,
with 10 ms sampling and device/session rediscovery. It is not a true-peak or
acoustic loudness meter. `scripts/test-output-meter.ps1` checks real WASAPI
session isolation, decoded timing, gain/volume, pause and seek; it plays tones.

Monotonic queue wait, first-PCM, decoder total and cache/publication timings,
selected priority, cancellation and result-discard messages are bounded to
recent diagnostic events and Windows debug output; they are not a universal
cold-WSL speed guarantee. The 38-second previously observed delay has not
been attributed to any one phase by a reproducible measurement.

## Code organization and design boundaries

- `main.cpp`: Windows entry point and Debug leak reporting.
- `player_ui.cpp`, `ui_probe.h`: window, controls and acknowledged test input.
- `paths.h`, `config.h`: path handling, search normalization and settings.
- `library.h`, `catalog.h`, `scan.h`: library model, deduplication, cache and scan lifecycle.
- `audio.h`, `playback.h`: decoder access and acknowledged playback/history transitions.
- `history.h`, `paged_vector.h`, `state_io.h`: persistence and immutable paged
  snapshots. Serialization and file writes run off the UI thread. Snapshotting
  shares record pages and copies the numeric navigation trail; subsequent record
  changes detach only their affected page. Library and history searches reuse
  normalized text; sorting reuses cached names.
- `analysis_dsp.h/.cpp`: portable streaming loudness/peak accumulator.
- `analysis.h/.cpp`: Windows decoder, pipe cancellation and waveform codec.
- `gain_policy.h/.cpp`: portable track tag policy and per-serial frozen decisions.
- `analysis_registry.h/.cpp`: portable summary registry, checksum and atomic IO.
- `analysis_queue.h/.cpp`: portable deduplication, priority and preemption.
- `analysis_service.h/.cpp`: bounded decode pool and independent registry/blob IO.
- `browse.h`, `preferences.h`, `feature_ui.h`: retained-history views, ranked
  filtering, settings, shell actions, waveform and peak widgets.
- `directory_filter.h`, `directory_ui.h`: subtree scopes, priority directory
  indexing, pins and a clipped one-level directory browser.
- `grouped_list.h`, `transport_layout.h`: cached grouped rows and bounded,
  persistent waveform-panel sizing.
- `tests/portable_analysis_tests.cpp`: platform-independent DSP, gain,
  registry and barrier-controlled scheduler checks (CMake/CTest on Linux).
- `tests.cpp`, `analysis_tests.h`, `feature_tests.h`: Windows playback/state,
  streaming decode, UI-facing analysis, browsing and feature regressions.

The navigation trail contains both successful visits and explicitly queued
future slots, distinguished in the history model and persisted as `QUEUED`
records until a successful load commits them as visits. A future A/B comparison can switch
sources on one decoder while restoring the playhead; two concurrent decoders
are an optional latency/synchronization tradeoff. Loudness matching would use
measured gain separately from the comparison transport. A/B comparison is not
implemented.

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

## Sampling workspace

Right-click the main waveform to **Open song in sample editor** or **Select sample on waveform**. Drag a range, adjust its edges and click **Open selection in editor**. The visible **Edit samples** button and Ctrl+M also open the current song.
The native editor browses audioharvest recordings, exact sample revisions and
collections. It provides stereo waveform zoom, frame-accurate boundaries,
undo/redo, beat/onset snapping, loop audition, metadata corrections and verified
lossless export. Drafts and pending commands survive closing and reopening.

See [sampling setup and workflow](docs/sampling.md) for the WSL server command,
connection, metadata scope, revision/conflict behavior and test instructions.


Listening history is chronological, with the newest entry at the bottom. Clicking
an existing history row replays that visit without adding another entry by default.
To log history-row replays as fresh listening visits, set this in `player.ini`
and click Refresh (or restart):

```ini
add_new_listening_history_entry_when_replaying_history_item=true
```

The default is `false`. Back/Forward navigation continues to revisit existing entries.
