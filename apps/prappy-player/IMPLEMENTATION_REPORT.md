# Windows implementation review, 2026-10-30

## Cold startup and editor restoration follow-up

The reported blank editor and delayed return to the player exposed multiple
blocking paths. Playback now owns libmpv on a serial worker; the window thread
submits bounded/coalesced controls and reads snapshots. Loaded metadata is
copied before readiness is published, and gain still precedes unpause. Source
scans and sampling clients retire without joining provider IO on the window
thread. Shared task state, notification fences, bounded executors and joined
process-lifetime workers prevent callbacks into destroyed owners. An OS provider
that ignores cancellation can still delay final process teardown after the
window and instance lock have been released; this is not a guarantee that every
network provider can be forcibly interrupted.

The editor immediately restores its saved source, drafts, selection, viewport
and cursor. A checksummed, source-bound local preview restores up to 30 seconds
of PCM and an overview while the backend is unavailable. Preview IO runs off
the window thread; stale generations and mismatched source/blob/decode identities
are rejected. Returning to the player persists player mode before saving, and
stopping editor transport preserves the saved cursor. Foreground PCM precedes
speculative overview work, and successful credentials are reused per connection.

Opening the real 7,186-track catalog also exposed saved offline flags preventing
the selected local waveform from loading until source scanning finished. Saved
alias-specific identities can now locate display previews independently; they
do not authorize playback gain. A failing regression reproduced this lookup
bug, and native service coverage checks cached waveform recovery with every
catalog entry offline, without opening a decoder or authorizing stale gain.

The scraper follow-up `8cb70bd` reuses one verified catalog read view per audio
request and memoizes FFmpeg version by executable identity. Source hashing,
cache validation and mutation checks remain intact. Its 341 offline consumer
tests passed, including barrier-controlled concurrent metadata/speculative
requests during blocked decoding. Disposable warm PCM requests improved from
10.9 to 4.2 ms; this does not attribute the reported historical 15/38-second
delays to one backend operation.

Executed Windows validation includes strict Release, Release static analysis,
Debug and Debug ASan builds; the full health scripts with CRT leak reporting;
native and portable CTest; and full editor browse/PCM/edit/save/audition/export/
reopen workflows against an isolated real service. Deterministic barrier tests
cover blocked playback ownership, coalescing, gain ordering, cancellation,
executor bounds and destruction before IO completion. ASan is not a race
detector. The expanded Debug test translation unit requires `/bigobj`; `/W4
/WX` and assertions remain enabled.

`verify-sampling.ps1 -WorkflowOnly -ColdRestart -AudioLoopback` held backend
responses until the restarted editor restored drafts/PCM and played, switched
to the player, and closed. A subsequent restart restored player mode and
responsive transport before release of the backend gate. Release editor/player
readiness was 307/318 ms, Debug 383/318 ms, Debug ASan 1,031/848 ms. These are
controlled restart measurements on this PC, not measurements of a fresh OS or
WSL boot. `test-output-meter.ps1` also passed actual WASAPI gain, mute,
pause/resume and seek checks.

Final strict builds succeeded for all four configurations. The latest complete
`scripts/verify-health.ps1 -SkipBuild -Repetitions 1` passed Release, Debug and
Debug ASan state/dedup/features/window/relaunch checks and Debug CRT reports
(`build/cold-start-health-pdb-all.log`). Earlier launch attempts were rejected
by Application Control despite valid Authenticode signatures. Local CMake linker
settings were rebuilt with full symbols, nonincremental Debug linking and
`/PDBALTPATH:%_PDB%`; subsequent launches passed without changing policy. This
sequence does not establish that a particular linker flag fixes reputation
decisions or guarantees acceptance of future builds.

Final `ctest --test-dir build/<variant> --output-on-failure` passed **2/2** in
Release, Debug and Debug-Asan. The final gated Release workflow measured
346 ms editor / 289 ms player readiness and passed selected-source loopback.
`test-output-meter.ps1` passed with gain ratio 0.501158, muted peak zero and
isolated WASAPI capture (`build/cold-start-final-validation.log`). The complete
editor workflow also passed (`build/cold-start-full-editor.log`).

After `scripts/build.ps1`, the exact pinned Release executable was reopened
normally as **PID 17688**, version **0.1.3**, Authenticode **Valid**, SHA-256
`BF97D3A778F31D77C68F8D10934D08DB38A8B20A8EAE55C16528D130377A4B5E`.
It restored player mode and the selected Mark Guiliana source. The saved waveform
was already visible while the real catalog scan was still at 3,067 files.
Actual source playback produced WASAPI RMS 0.0431226 over 4,800 captured frames
from a zero baseline; the player was then paused and left open. All **576 history
entries, 8 source workspaces and 25 drafts** were retained. Workspace identities,
draft IDs/ranges and saved-region bindings were compared before/after playback.
Evidence: `editor-final-reopen.json`, `editor-final-live-playback.json` and
`editor-final-pinned-release.png` under `build/`. Only the two owned disposable
cold-start servers were gracefully stopped; production service/collectors remain.

Previously documented Sound Check/reference calibration, full memory/phase
accounting and network-provider/race validation limitations remain open. No
old analysis cache, recordings, history, source identities, regions, drafts or
sample workspaces were removed. Windows security, manifest and DLL policy
remain unchanged; the existing free local development signing setup is used.

The existing user commit `7456762` was reviewed in place. This report and
`DELETIONS_AND_RENAMES.md` were missing from that checkout. New fixes are
follow-up commits; no archive was reapplied and no initial commit was recreated.

## Native environment and fixes

Windows x64, MSVC 19.44 / toolset 14.44.35207, C++20, CMake/Ninja, Intel
Core i9-12900KS (16 cores, 24 logical processors), about 192 GiB RAM. The private helper was found
at `C:/Users/Home/projects/dotfiles_private/windows/bootstrap_windows/native_cpp/NativeCpp.Common.ps1`.
SDL3, ImGui, ImPlot, pinned FreeType and libmpv are used by the native product.
WinHTTP/Core Audio linkage, the manifest, process isolation and DLL search
policy remain in place. The existing local development certificate setup was
used; no signing requirement or trust/security policy was added.

The initial native build exposed C4456 shadowed config variables under `/W4
/WX` and a missing `scheduleMissingLocked(false)` argument. These were fixed
in `65c6933`. Future ZIP filesystem timestamps caused Ninja's 100-regeneration
failure; correcting the local CMakeLists timestamp resolved that environment
issue without changing source content.

Further fixes:

- Measurement previously made synchronous metadata/codec property calls while
  its thread also owned the PCM pipe reader. On this pinned Windows libmpv,
  workers hung in property reads while audio threads blocked on pipe writes;
  the state test hung and the UI could not close. Measurement now observes
  metadata/codec events and copies event-owned data before the next event.
  Playback's returned metadata node is separately released by
  `mpv_free_node_contents` through its RAII owner.
- A constructor mutex fences publication of the worker vector before the IO
  or decode loops can read it. No vector mutation occurs after construction.
- The UI establishes the selected identity before draining warm completions.
  This fixes a reproduced strip-mode restart that discarded an early cached
  waveform. Completion validation uses the alias's actual scanned identity.
- Standard positive ReplayGain values such as `+7.00 dB` now parse correctly.
  A new test reproduced rejection before the fix and passed afterward.
  R128 parsing also accepts the RFC's leading `+`, enforces its six-character
  bound, rejects whitespace/multiple signs and rejects duplicate track-gain
  tags. These cases have regression coverage; the positive-sign test failed
  before the fix.
- A tag peak alone cannot authorize positive gain. Its source sample domain
  has not been verified after output gain, downmix and resampling. Matching
  decoded sample peaks still authorize boost subject to +12/-1 dB limits.
  A regression reproduced the unsafe authorization before this change.
- State checks sample libmpv's actual `volume-gain`, and the gain-decision
  callback checks that mpv is still paused. Playback test extraction keeps
  MSVC static analysis below its stack-use warning threshold, without
  suppressing warnings or removing checks.
- Windows builds now expose portable CTest and an explicit real-codec fixture
  executable. Test targets follow the ASan/runtime configuration so the
  diagnostic variant instruments them. UI/performance scripts use v2 caches.
- Playback normalizes filesystem separators at the mpv load boundary. CMake's
  forward-slash UNC fixture path failed before unpause until this correction;
  the same real-codec suite then passed without normalizing the test input.
  A real 268-character WSL source also failed with mpv's `Invalid argument`,
  while an extended UNC path and a shorter WSL alias loaded. The decoder now
  receives extended native absolute paths, including the tiny-FLAC retry,
  while playback/catalog identities retain their original paths. A nested
  >260-character UNC fixture failed before the fix and passed afterward.
  The running product subsequently repaired the failed real-source summary.
- Windows product/test version resources identify their actual executable
  names. Diagnostic test targets retain PDB symbols in Release as well.

## Commands and evidence

Run from `apps/prappy-player` unless otherwise stated. Detailed logs are local,
gitignored `build/review-*.log` artifacts; recordings and production state are
not test fixtures.

```powershell
pwsh -NoProfile -File scripts/build.ps1
pwsh -NoProfile -File scripts/test-dedup.ps1
pwsh -NoProfile -File scripts/test-state.ps1
pwsh -NoProfile -File scripts/verify-features.ps1
pwsh -NoProfile -File scripts/verify-window.ps1
pwsh -NoProfile -File scripts/verify-relaunch.ps1
pwsh -NoProfile -File scripts/test-output-meter.ps1
pwsh -NoProfile -File scripts/verify-health.ps1
```

The first complete health run passed native Release, static analysis, Debug,
Debug ASan, repeated playback stress, state/UI/window/relaunch checks and Debug
CRT leak reporting. A final run after the policy/recovery changes is recorded
in `build/review-health-final.log`; see the final validation status below.
State coverage includes quoted phrase search, chronological/default-reused
history visits, repeat/restore/rapid load/rollback, preferences, local editor
draft/workspace persistence, real pipe decode/cancellation, warm alias summary
reuse, selected corrupt-blob repair and registry checksum/backup/interrupted
replacement recovery. The UI suite restored strip/topmost/normalization/queue
state and the waveform together. No assertions were relaxed to make failures
pass: cache assertions now require three source/alias blobs and a separate
registry instead of the old two canonical-track cache files.

The audible WASAPI test passed with 174 tone samples, 116 silence samples,
zero timing mismatches, measured -6 dB ratio `0.501187`, and isolated loopback
660 Hz amplitude `0.0005749507` across 511,200 frames. A nonzero meter alone is
not treated as proof of stable normalization.

Generated codec fixtures are disposable real media, not references claimed to
have been authored by Apple or a calibrated ReplayGain tagger:

```powershell
wsl -d ubuntu26-dev -- python3 /mnt/c/Users/Home/projects/prappy/apps/prappy-player/tests/generate_analysis_fixtures.py /tmp/prappy-analysis-review-fixtures --ffmpeg /home/home/p/g/n/scraper/.venv/lib/python3.12/site-packages/static_ffmpeg/bin/linux/ffmpeg
# Configure under the imported MSVC environment, then build/sign with the normal scripts:
cmake -S . -B build/Release '-DPRAPPY_ANALYSIS_FIXTURES=//wsl.localhost/ubuntu26-dev/tmp/prappy-analysis-review-fixtures'
ctest --test-dir build/Release --output-on-failure
ctest --test-dir build/Debug --output-on-failure
ctest --test-dir build/Debug-Asan --output-on-failure
# Explicit invocation is also supported:
& build/Release/prappy_analysis_windows_tests.exe '\\wsl.localhost\ubuntu26-dev\tmp\prappy-analysis-review-fixtures'
```

The codec executable passed all eleven fixtures over WSL UNC. FLAC, MP3, OGG
Vorbis and M4A/AAC exposed track ReplayGain keys and codec data before
unpause; positive signed values applied +6 dB only after a measured peak was
available. Invalid/missing/conflicting FLAC tags and synthetic `iTunNORM`
rejected/fell back safely. Pinned libavformat combines conflicting duplicate
FLAC values into one semicolon-delimited value, which strict parsing rejects.
Opus exposed `R128_TRACK_GAIN=-512`; the -23 to -18 LUFS reference adjustment
selected +3 dB. Measurement of its patched +6 dB OpusHead control changed from
-20.0433 to -14.0433 LUFS and peak from 0.101600 to 0.202719: mandatory header
gain was applied once. All baseline measurements remained near -20 LUFS,
confirming optional ReplayGain/R128 was not applied by measurement.

The pinned dependency source DLL SHA-256 is
`04E4D27DC385DC18DDD93FBE69C75E635B864F3C6EC53A9122DF54C2F7842F4A`.
Runtime binaries may have an additional local development signature. The
dependency pin is the `20261021` / `e471496c56` artifact in `scripts/deps.ps1`.
The fixture generator used FFmpeg `n8.0.1-48-g0592be14ff-20260116` in WSL;
the decoder and product under test are native Windows libmpv/MSVC binaries.

## Benchmark and calibration limits

```powershell
# Always choose a new disposable cache directory for a cold benchmark.
& build/Release/prappy_analysis_windows_tests.exe --benchmark build/Release/player.ini build/Release/player-catalog.bin build/review-real-source-analysis
```

The production catalog was read only: 7,186 tracks. Four real WSL sources
were selected as a bounded benchmark, not a full-library rescan. With two
remote decode slots occupied, a selected 394.762-second MKA source waited
35 ms for admission and delivered in 743.876 ms; decode took 700 ms, first
PCM 29 ms, serialization 4 ms and artifact publication 3 ms. Its warm
selected waveform loaded in 2.7765 ms (blob read 2 ms), without a whole-song
decode. These are workstation observations under the current warm/running
WSL environment, not cold-distro or adverse-network guarantees. The historical
38-second delay has not been attributed.

A second run (`review-wsl-benchmark-final.log`) measured 32 ms admission and
722.633 ms selected completion for the same 394.762-second Opus source. Other
measured sources were AAC 260.876 s, Opus 60.047 s and 24-bit PCM 0.789 s.
The warm phase added two uncached real sources and observed both decoder slots
occupied before requesting the ready target. That cached waveform arrived in
23.941 ms total (2 ms blob read), before either background decode completed.
This directly exercises independent blob IO; it is still a small source sample.

The 3-second 1 kHz stereo PCM tone measured -19.9969 LUFS. On that same file,
FFmpeg's classic `replaygain` filter returned +5.86 dB and peak 0.099976; a
measured -18 LUFS adjustment would be +1.9969 dB. This is evidence against
claiming a universal 89 dB SPL == -18 LUFS conversion. The application's
existing ReplayGain reference alignment remains an explicit assumption.
The generated +6 dB fixture tags exercise policy, not calibration accuracy.
The original [ReplayGain calibration reference](https://replaygain.hydrogenaudio.org/calibration.html)
and [RFC 7845](https://www.rfc-editor.org/rfc/rfc7845.html) distinguish the
reference conventions. Sound Check conversion remains unsupported: no tested
Apple-authored fixture/reference pair was available locally. A synthetic tag
only verifies safe rejection and cannot justify guessed conversion constants.

## Remaining gaps

- Full native 7k-entry *measured* registry restart has not been benchmarked.
  Windows portable CTest exercises 7,159 synthetic ready records and
  barrier-controlled fake decoder priority/preemption; the real-source
  benchmark has four measured records. These are different validations.
- WSL was already running. No cold-distro restart, injected offline/changed
  network alias, slow source-open/pipe stall, real 256-request saturation,
  full-library CPU/memory benchmark or Windows race-detector run was performed.
  ASan is not a race detector. Synchronous filesystem identity checks and mpv
  destruction can still wait on an unresponsive source outside cooperative
  pipe cancellation. No upper bound is claimed for those waits.
- Retry classification/progressive backoff remains unfinished: persisted
  failures currently delay 60 seconds. Offline tracks retain catalog data,
  but the registry's Offline enum has no complete transition implementation.
  A separate metadata-only probe is not implemented; playback metadata and
  decode-event metadata are available without another whole-song probe.
- Identity reuse is still path/size/mtime based, not a reliable content
  fingerprint. Same-size same-timestamp changes need forced revalidation.
- Phase diagnostics now include codec/duration, queue wait, first PCM,
  decode, serialization and publication. `openCheckMs` is the identity check,
  not an isolated decoder source-open measurement. Total memory accounting
  does not cover concurrent accumulators, serialization buffers, libmpv
  allocations and all in-flight decodes; the waveform completion budget is
  not a process-wide memory cap.
- Source-domain tag peaks are conservatively untrusted. No true-peak limiter
  is provided. ReplayGain calibration across reference material and actual
  Apple Sound Check conversion remain open.
- Backend-dependent native sample save/reopen/regions/transport/workspace UI
  tests were not run: fixture manifests on disk point at local ports
  18766/18767/18768; explicit localhost TCP probes could not connect. No backend was
  started or modified. Local model/draft/workspace tests passed; those do not
  replace authenticated backend integration or actual editor output tests.
- Actual `volume-gain` was sampled across late-result/seek/pause/next/rollback
  state tests and on paused real-codec loads. UI tests exercise normalization,
  refresh, history, queue and restart behavior. No complete isolated WASAPI
  capture correlates every one of those actions to first audible output.

## Final validation status

Final native builds succeeded for Release, Release-Analyze, Debug and Debug
ASan. Final CTest passed **2/2 in each of Release, Debug and Debug-Asan**:
`portable_analysis` and `windows_analysis` with the real WSL codec fixtures.
Logs: `review-Release-ctest-complete.log`, `review-Debug-ctest-complete.log`,
`review-Debug-Asan-ctest-complete.log`. The ASan tests now compile with
`/fsanitize=address` and the static Debug CRT; generated Ninja flags were
inspected, rather than inferring instrumentation from the directory name.
Runtime reports `mpv v0.41.0-1227-ge471496c5`.

The final full `scripts/verify-health.ps1` run passed all three runtime variants,
all four native builds including static analysis, repeated playback stress,
state/UI/window/relaunch checks and Debug CRT reporting. The complete log is
`review-health-complete.log`. Final `test-output-meter.ps1` also passed: 171
tone samples, 116 silence samples, zero timing mismatches, gain ratio 0.501187,
660 Hz loopback amplitude 0.0002118012 over 513,120 frames.

Earlier attempts encountered the existing intermittent Application Control
rejection (Code Integrity 3077, status 0xc0e90002), including validly signed
builds. Per the user's instruction, work continued with source/resource changes,
native rebuilds, signing through the existing setup and fresh launch checks.
Rebuilt artifacts launched and completed the tests above. Accurate executable
identity metadata and diagnostic logging were retained; there is no claim that
a particular metadata field caused the reputation decision to change. No
security policy, trust store, sandbox, manifest or assertion was weakened.
Smart App Control's `VerifiedAndReputablePolicyState` remained **1 (enforced)**.

The rebuilt exact pinned executable is:

`C:/Users/Home/projects/prappy/apps/prappy-player/build/Release/PrappyPlayer.exe`

SHA-256: `365D837FD5E8CE4F60B176B13AC418051956F84C11DE1B67A0CD0AD6F8242B54`.
Authenticode status is Valid. After the final tests, the normal player closed
cleanly in 363.807 ms, checkpointing **4,717 real source records** (1,526,225
bytes). The Release target was rebuilt at the pinned path and normally reopened
from its own working directory. Process **62916** was the exact requested
Release executable. The native window appeared in 560.849 ms; rendered blue
and orange waveform channels were observed in 802.406 ms while background
analysis continued. These are observed startup/CPU capture times, not photon
latency. No Debug/alternate executable was substituted.

The restored source was `Nu Nu - Avishai Cohen, Mark Guiliana`, at its saved
position. Native Play/Pause input produced real WASAPI loopback RMS from zero
to 0.0004200433 (5,760 frames sampled), then it was paused again. The retained
history count stayed **523**. Evidence: `review-final-restart.log`,
`review-final-live-playback.log`, `review-final-pinned-release.png` and
`review-pinned-final-build.log`. The player remains open and paused.

Production recordings, history, catalog identities, UI preferences, sample
workspaces and both analysis cache namespaces were preserved. No production
player needed force-closing. The real-source benchmark wrote only new
disposable caches. Backend-dependent integration, the full 7k measured restart,
adverse-network shutdown and the implementation gaps listed above remain open.
The largest sampled live private-memory value was about 849 MiB during real
background decoding; this is further evidence that the configured waveform
budget must not be described as a process-wide allocation cap.

Follow-up commit IDs and the authorized `origin/main` push result are reported
in the final handoff after Git confirms them.

## Editor registration follow-up, 2026-10-29

The reported Take On Me failure was reproduced in the old real Windows player:
an unregistered source reached the editor with `mark_failed=true` and the exact
"not registered in a connected library" notice. Playback's filesystem catalog
and the sampling server's canonical capture registry were separate, with no
explicit editor registration bridge. Pending source selections also were not
checkpointed, and local-source deduplication replayed the journal per capture.

Explicit editor opens now resolve first and, when absent, register through the
authenticated, durable `source.import` command in the selected library. The server
requires configured source roots and a matching full-file SHA-256. It authorizes
the actual opened Linux descriptor and retains it through staging and mutation
verification, preventing file/ancestor symlink replacement from redirecting reads.
Originals remain untouched. Background playback preparation remains read-only.
The launcher permits the scraper's `library/` and phase-02 source trees; other
paths fail safely while retaining the requested range.

The player checkpoints the frozen path, action, cue, duration, selection, known
listening digest, import UUID and destination before registration. Listening hash
reuse requires the same playback serial and path. Restart/retry retains identity;
cancellation and superseding gestures reject late replies. Saved times are checked
against canonical frame limits before conversion. Previous source drafts remain
intact until the requested source resolves. The old failed selection was never
saved, so its original range cannot be recovered after the old process exited.

The backend now verifies canonical truth once for local-source deduplication.
One measured live HTTP deduplication request for the 243.960458-second Take On Me
MKA returned its existing capture in 1.632390 seconds with `new_file=false`.
This was a warm duplicate-import observation, not a controlled cold/warm comparison
and not an attribution of the historical 38-second delay.

Additional Windows findings: the session meter could report a residual
`2.15061e-05` peak while mpv volume was exactly zero. Explicit mute now disables
the UI meter like pause; the numerical assertion was retained and an inactive-meter
assertion added. The fixed real-device run reported zero muted peak, gain ratio
0.500955, correct silence/tone timing and real WASAPI capture. The GUI fixture also
now waits for the selection layout's rendered geometry. Combo navigation uses
player-owned one-frame key taps, acknowledged after release, so host scheduling
cannot turn a scripted tap into auto-repeat. Expected range/view assertions remain.

Executed validation and evidence in `build/`:

- WSL pinned `.venv-studio` pytest on `test_source_import_http.py`,
  `test_sampling_server.py`, `test_library.py`, `test_music.py`: **88 passed**.
  Includes deterministic pre-open/pre-stage/post-stage file/parent replacement,
  unchanged originals, in-place mutation, FIFO, scope/authentication/digest rejection,
  idempotency, resolve, PCM and region access. `bash -n prappy.sh` and diff checks pass.
- `verify-sampling.ps1 -WorkflowOnly -RegisterSource -AudioLoopback` passed
  with separate disposable libraries in pinned Release, Debug and Debug ASan.
  It verifies no background import, exact drag/edge/move/Escape handoff, one matching
  capture, unchanged original hash, selected-tone WASAPI output, sample save, two
  drafts, source switching and restart. Debug CRT reporting passed.
- Pinned Release `verify-sampling.ps1 -MarkOnly` passed cold/warm editor opens
  and playback of the actual Take On Me source through the live server, using
  isolated player state and retaining the previous draft; no test region was saved
  into the user's library. Evidence: `take-on-me-editor.log`.
- Native persistence tests first failed against the missing checkpoint and then
  passed; oversized saved-time conversion also had a failing regression before
  its guard. Native state tests cover restored identity/UUID/destination and
  same-path/different-serial digest fencing.

The sampling service was gracefully restarted with the updated launcher; collection
processes were untouched. Backend commit `4086ef4` was pushed to `origin/main` in
the scraper repository. Windows security enforcement and DLL loading policy remain
unchanged. Targeted Defender scans and retries were used during intermittent launch
blocks; these observations do not establish a general policy-acceptance guarantee.
The earlier analysis-specific calibration, adverse-network, 7k measured restart,
retry/offline and total-memory limitations above remain open.

Final verification used the normal pinned build directories. Strict Release,
Release-Analyze, Debug and Debug ASan builds passed. Then
`scripts/verify-health.ps1 -SkipBuild -Repetitions 1` passed the complete serial
state/dedup/features/window/relaunch suite, including Debug CRT reports. An earlier
concurrent validation run failed its idle-render assertion; the assertion was
retained and the complete suite rerun serially on an idle desktop. CTest passed
**2/2 in each of Release, Debug and Debug-Asan**. The final pinned
`test-output-meter.ps1` passed with gain ratio 0.501111 and muted peak zero;
`verify-sampling.ps1 -MarkOnly` again passed on the actual Take On Me source.
Evidence: `editor-registration-health-serial.log`, `editor-final-output-meter.log`,
`editor-final-take-on-me.log`. The expanded backend importer-consumer regression
run passed **335 tests**, including offline collector, curation, projects,
readiness and regions, without operating production collection jobs.

Finally `scripts/build.ps1` rebuilt the exact normal Release target, which opened
normally as **PID 39188**, version **0.1.1**, with Authenticode **Valid**:

`C:/Users/Home/projects/prappy/apps/prappy-player/build/Release/PrappyPlayer.exe`

SHA-256: `306498BC394EAC7E65F65A053E2BD18020235B4839E02CF58B02241597284FBA`.
Its restored editor was connected to the live artist-search library. Native
Play/Pause input played the user's restored source through real WASAPI:
baseline RMS 0, playing RMS 0.2684434509 over 4,800 captured frames, then paused
RMS 3.67055e-10 over 5,760 frames. The player remains open and paused.
**574 history entries, 8 source workspaces and 25 drafts** were retained; draft
IDs, ranges and saved-region bindings were compared before and after playback.
Evidence: `editor-final-reopen.json`, `editor-final-live-playback.json`,
`editor-final-paused.json`, `editor-final-pinned-release.png`.
Smart App Control remains enforced (`VerifiedAndReputablePolicyState=1`).
No tracked source was deleted or renamed, legacy worker/gain/cache execution
symbols were absent in the final source search, and the old runtime
`player-analysis/` cache was not deleted.
