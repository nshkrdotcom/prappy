# Player diagnostics

## Where to look first after a crash

The default installed executable is `apps/prappy-player/build/Release/PrappyPlayer.exe`
relative to the repository root. Its log is
`apps/prappy-player/build/Release/player-debug.log`, with backup
`player-debug.log.1`. The install script defaults to installing **Debug** into
this Release directory; use `player-build.json` to identify the configuration.
For other installations, normal state and logs live beside the executable.
`--evaluation <directory>` and `--window-test <directory>` use the supplied
state directory instead.

Both configurations now report to `%LOCALAPPDATA%\Prappy\Reports` independently
of the optional debug log. The application automatically starts a small local
collector, `PrappyReports.exe`, on **127.0.0.1:19876**. There is no cloud account,
external collection server, Windows service or Python runtime dependency.
Sentry Native 0.17.6 uses its external `native` Windows crash handler; its WinHTTP
transport sends only to this fixed loopback endpoint. Boost.Stacktrace 1.91.0
retains initial C++ throw locations, including third-party exceptions.

Run these from the repository root:

```powershell
Get-Content apps/prappy-player/build/Release/player-debug.log -Tail 80
pwsh apps/prappy-player/scripts/inspect-crash.ps1
# Or specify an incident ID/path:
pwsh apps/prappy-player/scripts/inspect-crash.ps1 -Incident '<incident ID>'
```

The analyzer uses archived binaries/PDBs, so a rebuild does not invalidate a
retained incident. Native reports show fault context, all thread stacks, source
lines and loaded-symbol details. Handled reports show the original throw trace
and complete exception chain. Hangs are detected after 15 seconds without a UI
heartbeat; the analyzer resolves the watched thread's blocked source location.
Intentional modal waits pause the watchdog. Optimized/inlined code can resolve
to the application's calling line instead of a separate library frame.

Files under `Reports`:

| Path | Contents |
| --- | --- |
| `incidents/<ID>/fatal.json` | Handled fatal error: UTC, PID/TID, build/session identity, operation, handler, exception chain and raw/resolved initial throw stack. |
| `incidents/<ID>/event.json` | Native/handled/ImGui/hang event, module identities and up to 128 recent breadcrumbs. |
| `incidents/<ID>/crash.dmp` | Native fault context and thread/memory data when a native crash occurred. |
| `incidents/<ID>/report.envelope` | Exact durable upload, including attachments; acknowledged only after persistence. Retries preserve identical bytes. |
| `incidents/<ID>/session.json`, `player-build.json`, `emergency.txt`, `phase.txt` | Attached lifecycle/build context, emergency/assertion identity, and the latest UI phase with its thread ID. |
| `sessions/<ID>/` | Startup and explicit exit status/reason; a pre-opened emergency file supports terminal failures without ordinary logging. |
| `symbols/<build ID>/` | Exact executable, PDB, reporting helpers and package manifest archived during build. |
| `sdk-cache/` | SDK persistence/retry store; bounded to 50 cached items/256 MiB. |

Incident retention is 50 reports/1 GiB, always preserving the newly collected
incident even if it alone exceeds the budget. Normal exits produce no incident
and do not displace failures. Lifecycle cleanup keeps 100 inactive sessions;
recent or live sessions are preserved. Symbol cleanup retains the newest ten
packages plus every package referenced by retained incidents. Unreadable
metadata stops symbol pruning rather than risking loss of matching symbols.
Permissions/disk failures are surfaced; evidence cannot be guaranteed if the
destination itself cannot be written. Reports, dumps and symbols remain local
and are ignored by Git.

For a deliberate full-memory escalation, capture the still-running player from
the separate helper process (choose a new output directory):

```powershell
& apps/prappy-player/build/Release/PrappyReports.exe --snapshot <PID> 'C:\path\to\new-snapshot'
& apps/prappy-player/build/Release/PrappyReports.exe --analyze 'C:\path\to\new-snapshot' "$env:LOCALAPPDATA\Prappy\Reports"
```

A live snapshot shows current stacks and full memory, not an implied original
exception context. Full snapshots are explicitly requested and outside automatic
incident retention; remove them when no longer needed. Routine collection caps
envelopes at 64 MiB and uses smaller native dumps. Keep diagnostic archives local:
process memory can include private data even though debug logs omit credentials.

Routine phase updates use a mapped attachment, not SDK scope mutations (which
write the SDK database synchronously). Breadcrumb producers enqueue into a
bounded background queue of 128 entries, with messages capped at 1024 bytes;
overflow drops the oldest queued entry and records a loss count. SDK breadcrumb
disk work stays on that worker. A handled fatal event waits at most 500 ms for
queued breadcrumbs before capture; an immediate native crash can lose pending
breadcrumbs but still captures its mapped phase and native fault stack. Fatal
exception records and emergency assertion identity bypass the optional log queue.

Build with `scripts/build.ps1`; reporting sources are fetched once at pinned
archive hashes by `scripts/reporting-deps.ps1`. Install with
`scripts/install.ps1` to validate all executable signatures and package hashes,
archive the replaced package, and preserve player state. The installer stops
only its local collector, after confirming the player is closed, before replacing
helper files. Normal launch restarts collection automatically.

The tracked acceptance suite runs in disposable state on loopback port 19877:

```powershell
pwsh apps/prappy-player/scripts/test-crash-reporting.ps1 -BuildVariant Debug
pwsh apps/prappy-player/scripts/build.ps1 -Config Release -VariantSuffix Reporting
pwsh apps/prappy-player/scripts/test-crash-reporting.ps1 -BuildVariant Release-Reporting
```

It requires original source locations, exception chains, archived-symbol native
analysis, a simulated SDL device-loss event through the actual D3D renderer (with
its device-removal HRESULT), ImGui error persistence, hang stacks, a full external snapshot, disabled
debug-log coverage, visible unwritable-destination failure, idempotent uploads,
normal/modal waits without false incidents, failure retention over normal restarts
and a bounded incident count. A locked-file probe requires retryable HTTP 503,
visible collector health status and successful recovery after unlocking.
The phase-load probe requires 100,000 UI phase updates with zero process file
write operations; native probes require the mapped phase attachment and durable
assertion identity.
Receipts and copied evidence are under
`build/reporting-validation/`; synthetic retention probes affect only that store.

The earlier ProcDump monitor from commit `d79336c` is retired. Legacy dumps
remain under `%LOCALAPPDATA%\Prappy\CrashDumps`; they are not deleted by current
startup. A legacy `Process termination` dump may be a normal close or handled
error with no original exception context. The preserved incident below describes
why this was insufficient. Unexpected OS fail-fast conditions still depend on
the SDK's WER integration and Windows policy; forced process kills and earlier
memory corruption cannot be guaranteed to yield an identifiable root cause.

## Reporting audit: 2026-11-11 incident

### Subsequent incident at 15:59:51 HST

Incident `e815e375a0a541bfa7ea9ff45658a5a1` captured the original throw in
`Workspace::monitorRange`, reached through `beginAudition` and the Space-key
transport action. The exception was JSON type_error.306: `value()` on null.
Evidence is preserved under `build/crash-20261111-155951-e815e375a0a541bfa7ea9ff45658a5a1/`.

The root defect was conflating restored editor state with loaded audio. A draft
and capture remain valid while a descriptor is absent or being replaced. Some
browser transitions also changed the endpoint before invalidating the old
descriptor/session. Transport actions had no common readiness requirement.

The editor now validates source identity, native clock, endpoint, coverage and
playback-owner key together before audition/resume/save. Descriptor replacement
invalidates transport and waveform context together, with generation checks
rejecting stale responses. Coverage-dependent waveform/cache polling shares the
same clock/coverage validation. Offline editing and seeking remain available;
unavailable playback preserves the draft/cursor and reports that audio is not
ready. Transport device/range errors stay in the editor. A valid descriptor with
a recoverable PCM error remains retryable, rather than being confused with a
missing descriptor. Lifecycle tests cover both unavailable states and successful
owner acquisition, not just the original null access.

### Evidence and limits

The incident at 14:47:53 HST (2026-11-12 00:47:53 UTC), PID 20708,
ran the Debug build of commit `499f714` installed in `build/Release`.
The dump, log and rotated backup, executable, matching PDB, build manifest and
debugger output were preserved in the repository-local ignored directory
`build/crash-20261111-144753/` before rebuilding or running probes.

ProcDump labelled this dump **Process termination**. It contains one remaining
thread and no exception stream/context. Its symbolized stack is
`ZwTerminateProcess -> ExitProcess -> exit_or_terminate_process -> common_exit
-> exit -> __scrt_common_main_seh -> wWinMainCRTStartup` (shown here in the
debugger's innermost-first order). Stack argument values are consistent with
exit code 1, but these are not an independently captured process exit status.
This supports a handled fatal exception followed by application shutdown;
it does not identify the exception or prove the exact original cause.

The final log records a catalog cancellation at 00:47:52.698Z and HTTP completion
at 00:47:53.002Z. `refreshPreparedCatalog()` catches that cancellation and
returns false; it does not propagate it into the UI fatal handler. `Scan`'s
destructor requests cancellation during shutdown. Therefore the cancellation
can be a consequence of shutdown and is not evidence that it caused the exit.
Application events in the recent incident window and System events around the
incident yielded no corroborating event. The retained dump cannot reliably
recover an already-unwound throw stack; searching its strings did not establish
an active exception message. The root cause remains unconfirmed.

### Deficiencies at the time of the incident

| Gap | Consequence |
| --- | --- |
| Outer `runPlayer`/`wWinMain` handlers showed `what()` only in a dialog | The most useful error message was absent from the incident log. |
| ProcDump uses `-e -t`, without first-chance capture | A caught C++ exception is not captured when thrown; the termination dump arrives after stack unwinding and object destruction. |
| Dumps on normal termination and only two previous dumps retained at startup | Ordinary closes and verification runs can displace the actual failure evidence. |
| Debug-only crash monitor | A true Release package lacks this capture path. |
| Optional, asynchronous, bounded debug log | Disabling logging, an unwritable destination, a full queue or a sudden crash can lose evidence. `flush()` waits for the worker but does not guarantee successful or durable disk writes. |
| Each debug row is truncated to 766 bytes | Long exceptions or stack traces can be silently cut off. |
| No durable incident ID, explicit fatal category, operation/phase or thread ID in normal records | Correlation and distinction between a normal close and a handled fatal error require guesswork. |
| ImGui error callback is not connected to persistent diagnostics | Its built-in debug log/tooltip is not a durable incident report. |
| Several renderer/text failures throw only `SDL_GetError()` | Even a retained message may not identify which API/operation failed; GPU removal reason is not recorded. |
| Dump inspection helper exists only as a local build artifact | Another checkout cannot depend on a documented, reproducible analysis command. |

Matched-symbol installation is already a useful safeguard: the preserved PDB
successfully resolves this executable. The missing piece is capturing the
failure before its context disappears, plus retaining a complete incident.

### First mitigation (before the complete implementation above)

The initial mitigation logged and flushed `app.fatal` with handler and exception text
before showing either outer error dialog, and logged/flushed `app.exit code=N`
after normal UI cleanup. A Debug-only `PRAPPY_TEST_CRASH=handled-exception`
probe, limited to the existing `--window-test` path, exercises this handler.
The Debug build succeeded, including development signing verification.
An isolated window-test state directory recorded the injected message before
dialog dismissal, then recorded and returned exit code 1. Evidence is under
`build/crash-20261111-144753/handled-probe-verified/`.

That was a small mitigation: it used the optional debug logger, lacked throw-site
stacks, and could run before logging was configured. It was built and tested in
`build/Debug` before the complete reporting implementation described above.
Neither change can recover the missing message from the earlier incident.

### Design requirements from the initial audit

The implementation above follows two coordinated reporting paths: fatal C++ error events with throw-site
traces, and native crash dumps captured by an external handler. Both should
produce an incident directory that can be inspected locally.

1. **Make fatal evidence independent of verbose logging.** Initialize a small
   incident writer before normal startup. Write the error category, complete
   exception chain/type/message, PID/TID, UTC time, operation/phase and incident
   ID before showing a dialog or destroying application state. Use a bounded,
   best-effort emergency write path independent of the debug worker and its
   mutexes. Record failure to create the report visibly. Include explicit normal
   exit and fatal exit reasons. Handle non-`std::exception` C++ failures too;
   do not attempt to recover from arbitrary access violations as C++ errors.
2. **Capture C++ stacks at throw time.** Evaluate Boost.Stacktrace's
   `boost_stacktrace_from_exception` on this exact MSVC/static-runtime build;
   its documented `from_current_exception()` retrieves the initial throw trace,
   including third-party exceptions, on supported configurations. Require a
   proof test before adopting it. For application-owned errors, a C++20
   `std::source_location` wrapper plus captured raw stack addresses is a smaller
   fallback. Add operation names and error codes/HRESULTs at SDL/DirectWrite/
   D3D call sites, including `GetDeviceRemovedReason()` for applicable GPU
   failures. A stack captured only inside the outer catch identifies the catch,
   not the original throw. The project currently targets C++20, so C++23
   `std::stacktrace` is not a drop-in fix, and would still need throw-time use.
3. **Use an out-of-process native crash handler in distributed builds.**
   Crashpad is the preferred focused library candidate for local dump capture;
   evaluate Sentry Native if hosted aggregation, breadcrumbs and symbolication
   are desired. Neither replaces explicit reporting of handled C++ failures.
   Microsoft recommends a separate process for `MiniDumpWriteDump` where
   possible. Avoid a new custom in-process crash handler that allocates,
   locks or symbolizes while the process may be corrupted. Keep ProcDump as a
   development/reproduction tool. WER LocalDumps is an alternative workstation
   safety net, not a remedy for deliberately caught exceptions.
4. **Connect ImGui diagnostics without hiding assertions.** Register the pinned
   version's internal `ImGuiContext::ErrorCallback` to persist the error,
   current window and frame/operation context. Keep development assertions
   enabled. The existing editor stack-store/recover/restore pattern agrees with
   ImGui guidance for exceptions; recovery is explicitly not guaranteed.
   Ensure assertion/abort/terminate paths reach the native report mechanism,
   and persist an assertion's file, line and expression where available.
5. **Retain and symbolize complete incidents automatically.** Bundle the dump,
   recent bounded breadcrumbs, fatal event, exact build commit/configuration,
   module identities and manifest under one incident ID. Archive binaries/PDBs
   by immutable build identity. Retain failures separately from normal exits
   using age/size limits; do not prune the last failure on routine startup.
   Provide a tracked collector/analyzer that emits the failing thread,
   exception code, source frames and matching-symbol status. Full dumps can
   be an opt-in diagnostic escalation; increasing dump size cannot recreate
   the stack already destroyed by a handled exception.

For a short reproduction session, ProcDump `-e 1` can capture first-chance
exceptions before unwind. Use filters and a deliberate capture budget: expected
exceptions can otherwise exhaust `-n 1` before the relevant failure. Do not
turn every handled exception into an always-on full dump.

Acceptance tests must check report contents, not merely that a `.dmp` exists:
inject a nested handled C++ exception and require its named throw function/source
line; repeat with a third-party exception; test access violation, assertion,
abort/terminate and worker failure; test a normal close produces no fatal
incident; verify retained failure survives restarts/normal exits and symbol
resolution after rebuilding. Test disabled debug logging and an unwritable
report destination. Exercise both Debug and the actual shipped Release package.
GPU-device-loss and hang capture need separate controlled tests. A useful
report should identify the operation and first relevant application frame from
one documented analysis command; no crash system guarantees the ultimate root
cause for prior memory corruption or forced termination.

### Primary references reviewed

- [Microsoft ProcDump options](https://learn.microsoft.com/en-us/sysinternals/downloads/procdump):
  first-chance versus unhandled exception triggers, termination and dump sizes.
- [Microsoft MiniDumpWriteDump guidance](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump):
  external capture and preserving exception context.
- [Microsoft WER LocalDumps](https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps).
- [Dear ImGui error handling](https://github.com/ocornut/imgui/wiki/Error-Handling):
  callback, assertions and scoped recovery limitations; checked against the
  project's vendored 1.93.0 implementation.
- [Boost.Stacktrace exception traces](https://github.com/boostorg/stacktrace/blob/develop/doc/stacktrace.qbk)
  and [Windows implementation](https://github.com/boostorg/stacktrace/blob/develop/src/from_exception.cpp):
  supported runtimes, throw-time capture and overhead; adoption remains untested here.
- [Crashpad design and Windows limitations](https://github.com/chromium/crashpad/blob/main/doc/overview_design.md).
- [Sentry Native implementation/options](https://github.com/getsentry/sentry-native/blob/master/README.md):
  external handlers and Windows fast-fail/WER integration. These are candidates
  reviewed for suitability, not libraries integrated by this change.

## Log contents

`debug_log=true` defaults on. Set it to false and restart to disable logging.
`player-debug.log` beside the executable rotates at 1 MiB with one backup.
Records contain UTC time, elapsed milliseconds and PID. A bounded background
writer keeps log IO off the window thread. Credentials and request bodies are
excluded.

Useful events:

- `play.request/submit/loaded/ready`: user intent, decoder submission, matching
  FILE_LOADED and acknowledged playback. Serial IDs reject superseded loads.
- `play.preparing/prepared`: retained not-ready intent and its preparation wait.
- `catalog.ready/error`: catalog request duration, source count or failure.
- `backend.credentials/start`: token-file wait and local WSL backend launch.
- `http.dequeue`: request-lane queue delay.
- `http.begin/headers/server/end`: service wait, server duration, transfer and
  processing, status and success. `http.failure` includes Windows error codes.
- `artifact.verify.begin/end`: SHA-256 verification of a downloaded export.
  This event is unrelated to original-source verification.

Ready player/editor opens read no original audio and perform no source hash.
Background import/preparation logs carry the heavy original-file work. WSL HTTP
logs live at `~/.local/state/audioharvest/prappy-debug.log` and
`prappy-server.log`. `--no-debug-log` disables HTTP performance logging.
Preparation service logs use:

```bash
journalctl --user -u audioharvest-preparation.service
```

HTTP request IDs correlate the player's PID/request counter with the backend.
Explicit imports log `source.copy`, `source.decode.begin/end` and `source.verify`.
The normal import hashes while copying once, verifies stable file identity and
reuses that verified staging evidence. An exceptional changed descriptor can
require an additional hash. Background preparation independently checks its
registered original digest before rebuilding an asset.

## Actual long-recording evidence

Source: 147,407,042-byte original, 8,023.806 seconds (2 h 13 min). Its permanent
native 48-kHz stereo WavPack master occupies 1,802,733,782 bytes. Complete
background preparation/verification took 79.622 seconds in the measured store.
Those costs occur before Ready publication.

At the native pipeline checkpoint, 100 random-position Windows editor opens:

| Condition | p50 / p95 / p99, milliseconds |
|---|---:|
| Warm | 126 / 185 / 218 |
| Advisory converted-file page eviction | 134 / 216 / 353 |
| Concurrent complete background conversion | 127 / 188 / 312 |

Each opening includes four scripted input acknowledgments, roughly 100 ms total.
Page eviction uses `posix_fadvise(DONTNEED)` on isolated converted files; this
is not a physically cold-disk or cold-WSL guarantee. Concurrent conversion was
real work and took 79.390 seconds. Windows HTTP/native-reader random three-second
reads measured 15.61 / 23.65 / 32.08 ms when warm.

The editor verifier also reproduces temporary not-ready responses, automatic
queued playback, native one-frame trim/undo/redo, save/export, audition and
restart. Detailed receipts, limitations and representative corpus sizing are in
`~/jb/docs/20261104/prappy_scraper/04-implementation-and-validation.md`.

## Earlier registration diagnosis

Before the redesign, registering this recording took 19.03 seconds; reaching
first editor PCM took 37.90 seconds because it decoded the complete source
again. Reusing the verification output removed the duplicate pass (19.08
seconds through first PCM; verified duplicate registration 278 ms).
That historical cache implementation has since been deleted. Its measurements
explain the original bottleneck; they do not describe the current Ready path.
