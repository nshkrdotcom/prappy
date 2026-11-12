# Memory ownership, lifetime and concurrency repair — 2026-11-11

This document describes **implemented source changes**, evidence available from this source-only snapshot, executed checks, and Windows checks that **have not** run. It is a handoff/verification ledger, not a claim that the Windows application is validated. The distributed snapshot did not include Git history, binaries, symbols/PDBs, crash dumps, installed Windows dependencies, or the original state and music library. Do not infer original crash causes from these source defects alone.

## Scope, owners and shutdown dependencies

| Component | Owner / consumer | Lifetimes and boundaries examined |
| --- | --- | --- |
| `PcmBudget`, `NativeSession`, `Clip`, `CompressedSpill` | UI-owned playback/session holds budgeted immutable native PCM; clips and in-flight save/hash requests independently hold the exact spans/backing files they need | PCM reserve/adopt/retry, authoritative selection extension, cold spill/rehydration, reader failures, cancellation, source switches, cache eviction, save and last-snapshot release |
| `Audition`, `NativePlayback`, `Audio`, `Workspace` | SDL callback borrows immutable buffer pointers while stream is locked; player/editor transfer shared session/control, but source identity/coverage belongs to descriptor | Partial stream initialization, play/seek/loop/stop, pending descriptors, handoff, replacement, return, rejected/late replies and teardown |
| `Client`, `ClientExecutor`, `Scan`, `RemoteWaveform` | Client owns callback state and bounded queues, global HTTP executor owns worker lanes; scan has a separate global worker, scan result/future, and cancellation token | Worker/static initialization, pending/coalesced requests, in-flight cancellation, retirement/late notification, retries, large abandoned replies and shutdown joins |
| `Workspace`, `PagedVector`, persistence | UI owns drafts and editable paged history; history readers borrow shared copy-on-write pages, persisted state survives playback failures | Mutation exceptions, save/reload retries, dropped/stale replies and shutdown/persist separation |
| `report_collector.cpp` | Local listener owns socket, singleton/event and WinSock lease; analyzer owns mapped dump, DbgEng COM interfaces, callback attachment and DbgHelp state | Failed initialization, early HTTP exits, disk errors, very large dumps, exceptions during analysis and normal shutdown |

Other inspected first-party areas include prepared input/PCM conversion, gain policy/DSP, sample editor, sample library, source handoff, output meter, WinHTTP upload/hash, state I/O, rendering/font/graphics caches and diagnostics. No raw pointer or shared allocation is labeled a leak solely because of its type. Vendored third-party source was not modified. The only discovered `AGENTS.md` was in vendored SDL, outside the edited scope.

## Confirmed findings and implemented remedies

Severity describes the impact of the *failing transition*, not proof that a production crash originated there. Portable tests use the actual `NativeSession`, budget, `Clip`, `PagedVector` and prepared-clock implementations and inject failures only behind their storage/loader interfaces.

| ID | Severity | Evidence / failing transition | Applied repair | Evidence / regression coverage |
| --- | --- | --- | --- | --- |
| M01 | High | `NativeSession::retain()` wrote several compressed chunks to the live spill file before committing the cold index; a later failed write or allocation left charged storage with no reachable chunk. | Stage in a forked backing, reserve index space before commit, skip overlapping staged/cold coverage, publish after success, keep resident authority on failure. | `native_lifetime_tests.cpp`: second-store fault, quota returns to zero, preserved PCM readable, successful retry releases RAM and file quota. |
| M02 | High | Lazy `Clip::each()` wrote into the persistent writer before finishing a pass; failure could pin abandoned storage, and an arbitrary loader buffer/permit could be accepted as a selected native span. | Separate per-pass backing transaction, enforce complete native clock/span/permit provenance, validate stored byte extent before offset arithmetic; retain completed chunks for retries. | Tests for save fault/retry, cancellation/retry, preserved bits after session close, mismatched stored extent. |
| M03 | Medium | `NativeSession::adopt()` allowed overlapping resident replies to consume extra PCM and update decoder epoch even when a duplicate region was already retained. | Reject overlap and wrong-budget permits before publishing; stage decoder identity before commit. | Duplicate/foreign-budget tests; native handoff test retained for Windows. |
| M04 | High | `PagedVector::push_back` published an empty page before its first append; exceptions caused ghost pages, invalid later indexing, or incomplete copy-on-write publication. | Prepare first page/clone and element before publication; commit count only on success. | Injected move/copy failures, old-snapshot invariance, new-page retry. |
| M05 | High | `Audition::start/loop/stop` could retire the last large shared PCM owner while holding SDL's stream/callback lock. | Move previous owners to local UI-thread variables under the lock and release after unlock; no new callback allocations or I/O. | Source review; Windows SDL callback timing/device teardown still required. |
| M06 | Medium | `NativePlayback` destructor/`stop()` could propagate a failed lease-close submit; failure also risked skipping output/session teardown. Handoff could partially change owner before allocation finished. | No-throw destructor with diagnostic, retryable close path, stage next owner/session/lease, retire PCM/output on stop even on close failure; set `pending` only after successful submit. | Source review; Windows lease-queue saturation, session renew, seek/reopen tests required. |
| M07 | High | Editor drafts and parsed descriptors were treated as playback readiness; different blob/clock/coverage or absent session could reach a transport action. | Validate capture/hash, timeline, sample type/channels/rate/frame extent, descriptor range, live session, endpoint, playback error before reuse/play; preserve drafts and reload only the original source when returning from editor. | Existing `native_handoff_tests.h` / `sample_workspace_tests.h`; **Windows execution required**. |
| M08 | Medium | An exception while closing native playback skipped `Workspace` persistence; malformed replies dropped later results in the same collected batch. | Separate `closeNativeSession` and `persist` destructor actions; clear session references despite close errors; handle/log individual workspace replies. | Source review; persisted-state, malformed-reply and retry Windows tests outstanding. |
| M09 | Medium | UI waveform parsing multiplied unvalidated bin/channel offsets, cleared the last good peaks before validating replacement, and rounded huge `int64` frame counts via overflow-prone `n+stride-1`. | Validate channel count, bin count, offsets, extrema; build peaks transactionally; use overflow-safe `ceilDivPositive` for source-clock binning. | Portable `INT64_MAX` bin arithmetic tests; Windows overview/detail stale/malformed reply tests outstanding. |
| M10 | High | Client's uncollected **completed** replies could retain unlimited PCM and immutable snapshots even though pending requests were bounded; `ClientExecutor::retire` freed large replies while holding the global scheduler lock. | Cap pending+completed to 512, bound retained server-lease cleanup queue to 512, retire callback then swap discarded payloads out of lock, preserve active cancellation and server TTL fallback. | New deterministic completed-queue test in `sample_client_cancel_tests.h`; **Windows execution required**. |
| M11 | High | `scan_detail::Executor` constructed HTTP executor in the scan singleton's constructor body. C++ static destruction is reverse initialization order, so HTTP could shut down **before** scan workers joined, despite comments claiming the opposite. | Initialize HTTP executor before scan singleton construction, join scan first at shutdown, destroy abandoned library snapshots outside scheduler lock; fence callback exceptions and submit future publication. | Source construction-order analysis; existing `scan_async_tests.h` and Windows process shutdown tests required. |
| M12 | Medium | `RemoteWaveform::select` published an identity before submit, so a full request queue could strand it forever; polling submitted without a retry guard. | Surface submit error and retry the same generation after a short delay while preserving source identity; stale-generation replies remain ignored. | Windows queue-saturation/recovery tests required. |
| M13 | Medium | Collector early exits leaked WinSock sockets, events and mutex handles; DbgEng interfaces and symbol resolver lacked exception-safe cleanup, and full-dump analysis read a whole multi-GB dump into a new string. | Local RAII for HANDLE/SOCKET/Winsock, memory-map dumps, WRL `ComPtr`, unhook stack callback before destruction, scoped `SymCleanup`, enforce Win32 file-write limit. | Existing `test-crash-reporting.ps1`, probe/analyzer/full-memory-snapshot suite; **Windows execution required**. |
| M14 | Medium | HTTP request body serialization could exceed WinHTTP's `DWORD` send limit; debug capture of huge audio seek times could overflow `llround`. | Bound ordinary JSON requests to 8 MiB before `DWORD`; clamp long-double cursor before integer conversion; guard and clamp player queued seek. | Portable clock overflow tests, source review; Windows input tests required. |

Implementation files: `src/native_session.h`, `src/native_playback.h`, `src/native_spill.h` (inspected, unchanged), `src/sample_audition.h`, `src/audio.h`, `src/sample_workspace.cpp`, `src/prepared_audio.h`, `src/paged_vector.h`, `src/sample_client.cpp`, `src/sample_client_cancel_tests.h`, `src/remote_waveform.cpp`, `src/scan.h`, `src/report_collector.cpp`, `CMakeLists.txt`, and `tests/native_lifetime_tests.cpp`.

## Verification performed in this container

Host: Debian GNU/Linux 13, Linux x86_64 6.18.57; GCC 14.2.0; Clang 17.0.0; CMake 3.31.6; Ninja 1.12.1. Linux cannot compile or execute this project's MSVC x64, WinHTTP, DirectWrite/D3D11, Windows-specific SDL driver, Win32 minidump/Sentry collector or native GUI binaries. `pwsh`, `cl.exe`, Wine and MinGW were not available. Source snapshot omitted the pinned FreeType source, compiled MPV runtime, Sentry/Boost runtime packages, and Windows package identity artifacts. No credentials, existing user drafts, production WSL library, or backend were accessed.

| Status | Exact command and observed result |
| --- | --- |
| **PASS** (after fix) | `cmake -S apps/prappy-player -B /mnt/data/prappy-linux-build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build /mnt/data/prappy-linux-build --parallel 4 && ctest --test-dir /mnt/data/prappy-linux-build --output-on-failure` — all 3 tests passed. Initial source-only attempt was **FAILED** under GCC `-Werror=misleading-indentation` in `native_session.h`; the two ambiguous statement sequences were separated and the build re-run successfully. |
| **PASS** | `cmake -S apps/prappy-player -B /mnt/data/prappy-asan-build -G Ninja -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'`; build with Ninja; `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir /mnt/data/prappy-asan-build --output-on-failure` — 3/3 passed; no ASan/UBSan/LSan errors reported. |
| **PASS** | Separate GCC ThreadSanitizer configure/build using `-fsanitize=thread -fno-omit-frame-pointer -g` and `-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread`, then `TSAN_OPTIONS=halt_on_error=1 ctest --test-dir /mnt/data/prappy-tsan-build --output-on-failure` — 3/3 passed. Portable tests are primarily single-threaded and this is **not** proof of UI/HTTP race freedom. |
| **PASS** | `cmake -S apps/prappy-player -B /mnt/data/prappy-clang-release-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++`; build and `ctest --test-dir /mnt/data/prappy-clang-release-build --output-on-failure` — 3/3 passed. |
| **PASS** | `git -c core.whitespace=cr-at-eol diff --check` on edited source relative to a temporary, uncommitted baseline of the supplied snapshot. Existing CRLF formatting was preserved in modified CRLF files. |
| **BLOCKED / NOT RUN** | Windows Debug/Release full build, native DLL/SDL/MSVC analysis, Windows-based `ctest`, scanner/HTTP/collector and GUI fixture tests, CRT/PageHeap/AppVerifier, signing, package install and exact PDB/build-ID correlation: required Windows tools/fixture media and preserved incident evidence are unavailable here. |

These portable results test relevant real production logic with controlled fake persistence, but do not execute any Windows-specific change. They do not prove leak-free, race-free native audio playback.

## Windows integration, unresolved risks, and evidence protection

1. Use the original Windows checkout and **first inspect its current committed diff**. Read its `AGENTS.md`, `docs/diagnostics.md` and the `scripts/` command parameters. Preserve matching build manifests, PDBs, incident dumps, logs, prior executable package and user state before any rebuild, signing or install. Do not mutate a real music library or user drafts; use `tests/create_cold_start_fixture.py` and isolated fixture backend/state.
2. Run `pwsh scripts/deps.ps1`, `pwsh scripts/build.ps1 -Config Debug`, `ctest --test-dir build/Debug --output-on-failure`, `pwsh scripts/test-state.ps1 -BuildVariant Debug`; repeat for Release. Follow up with `pwsh scripts/build.ps1 -Config Debug -Asan`, `ctest --test-dir build/Debug-Asan --output-on-failure`, `pwsh scripts/build.ps1 -Config Debug -Analyze`; consult `scripts/verify-health.ps1` for CRT leak reports and existing end-to-end repetitions. Never treat an ASan-only run as sufficient.
3. With a **new disposable fixture manifest** (not production port 8765), run `pwsh scripts/verify-native-editor.ps1 -Manifest 'C:\absolute\disposable-fixture.json' -BuildVariant Debug -VerifySaveExport -VerifyCommands`, including `-Iterations`, `-EvictSourcePages`, `-InjectAnalysisPending` and `-InjectCatalogOffline` runs as justified. Exercise returned editor positions near coverage boundaries, malformed/out-of-order descriptors, duplicate PCM replies, storage quota failure, cancellation during upload and still-live save snapshot after player closure. Verify hashes/native frame coordinates and retained drafts before/after failure and retry.
4. Run the source-provided scanner and client tests plus `pwsh scripts/verify-relaunch.ps1 -BuildVariant Debug` and `pwsh scripts/test-crash-reporting.ps1 -BuildVariant Debug` (isolated collector **127.0.0.1:19877**). Explicitly test scanner lifetime against HTTP executor shutdown and notification throwing; queue saturation/recovery; `ComPtr` output callback detach, collector early exits, and large mapped-dump analysis. Production reporting remains **127.0.0.1:19876** only. Compare correct build-ID manifest/PDB/symbols, and do not replace preserved evidence.
5. Run targeted native leak/race diagnostics: MSVC `/analyze`, `/fsanitize=address` where supported, debug CRT leak reporting using the documented harness, and process-specific Application Verifier/PageHeap only on disposable test processes when authorized. Measure SDL callback latencies, PCM/spill quota under repeated handoffs, bounded queues, editor memory/handle growth, and synchronization/lease cleanup at process exit. Linux sanitizer status does **not** settle these Windows items.
6. `pwsh scripts/install.ps1 -Config Debug` follows a successful signed build and the script checks signatures, package hashes and state preservation; inspect `docs/development-signing.md` before any signing changes. Verify normal startup and shutdown after installation. This environment did **not** run any of these steps.

**Known limitations, not confirmed regressions:** Windows-only lifetimes need native compilation and concurrency/reproduction evidence; the impact of releasing large PCM owners just outside the SDL lock must be measured with a real device. HTTP lease cleanup is intentionally bounded and may rely on backend TTL when queues are saturated; test server behavior. A third-party native callback that throws a non-`std::exception`, incomplete DbgEng symbol setup or a malformed multi-gigabyte dump requires incident-specific verification. The UI `llround` coordinates from plot gesture input and other backend-driven JSON layout sizes warrant fuzz/ASan review if a real failing input emerges. None of these uncertainties is presented as a proven leak.

No Git commit, push, Windows installer, third-party dependency download, genuine audio playback, or signing was performed in this sandbox.


## Windows integration — 2026-11-11

This section supersedes the Linux snapshot's pending Windows status above. The
original incident is preserved; the source findings alone do not establish that
incident's root cause. Validation uses generated Opus/WAV media and an isolated
WSL backend on port 18861, never the production library or port 8765.

Evidence root: `build/memory-validation-20261111-165240` (also recorded in
`build/memory-validation-current.txt`). It contains the previous installed
package/state, preserved incidents, exact build/test logs and Code Integrity
launch failures. Existing build-ID symbol archives remain under LocalAppData.
Build logs show the actual compiler/toolchain and development signature checks.

Additional implemented corrections:

- Persist the cue preview's native selection range even when no draft exists;
  previously a restart reset it to the first four seconds. Add a real workspace
  persistence regression and verify repeated random-position editor restarts.
- Keep source readiness independent of a recoverable transport error, retaining
  all identity, clock, format, session and coverage checks. Verify failed PCM
  playback can explicitly retry instead of becoming permanently unavailable.
- Update native fixtures to supply the required hash/channel/format identity;
  reuse retained PCM in the integration test instead of adopting duplicates.
- Remove MSVC analysis-reported oversized stack allocations from leak reporting
  and handoff tests. Leak-report path storage uses the process heap and is freed
  before CRT enumeration, so the instrumentation does not count itself.
- Validate mapped dump headers, directory and stream extents before DbgEng or
  DbgHelp access; use aligned exception-stream copies. A truncated dump returns
  a visible error instead of reading outside its mapping.
- Fence nonstandard HTTP completion-notification exceptions at the worker
  boundary. The regression throws from one notification, then confirms the
  worker delivers the next notification and retains both completed replies.
- Run health verifiers in separate PowerShell processes: their script-scoped
  process/input variables otherwise escape into the parent runner and readiness
  checks can time out despite a ready application. Release validation uses
  `Release-Memory`, preserving the installed `Release` directory. ASan validation
  selects the built `Debug-Asan` variant. `-Variants` supports targeted resumes.
- Record private bytes, kernel/GDI/USER handles and accounted PCM/budget at each
  editor iteration, failing if the PCM budget is exceeded.

Executed commands (from the repository root; scripts consume the generated
absolute manifest/state paths):

| Result | Native Windows check |
| --- | --- |
| PASS | `pwsh apps/prappy-player/scripts/build.ps1 -Config Debug`; signed MSVC x64 build; `ctest --test-dir apps/prappy-player/build/Debug --output-on-failure`: 6/6. |
| PASS | `pwsh apps/prappy-player/scripts/build.ps1 -Config Release -VariantSuffix Memory`; signed build; CTest in `build/Release-Memory`: 5/5. |
| PASS | `scripts/test-state.ps1 -BuildVariant Debug` and `Release-Memory`: native ownership, scan cancellation/coalescing, 512-message pending/completed bounds, handoff, persistence, source readiness, SDL playback and history checks. |
| PASS | `scripts/build.ps1 -Config Debug -Analyze`: MSVC analysis after fixing C6262 findings, without suppressing warnings. |
| PASS with launch retry | `scripts/build.ps1 -Config Debug -Asan`; ASan state suite passed. CTest initially could not start `prepared_audio`; the unchanged executable and targeted CTest both passed on retry. This was a launch failure, not a sanitizer finding. |
| PASS | `scripts/test-crash-reporting.ps1 -BuildVariant Debug`: original throw/fault symbol correlation; native faults, worker, ImGui and hang reports; full-memory mapped snapshot; visible disk failures/retry; idempotency and bounded retention. Disposable collector port 19877. |
| PASS | `scripts/verify-relaunch.ps1 -BuildVariant Debug`: old/new window cleanup overlap and independent disposable instance locks. |
| PASS | `scripts/verify-native-editor.ps1 -Manifest <fixture> -BuildVariant Debug -VerifySaveExport -VerifyCommands`: exact native frame/identity save/export and committed-save reply-loss recovery. |
| PASS | Same editor verifier with `-Iterations 10 -EvictSourcePages -InjectAnalysisPending -InjectCatalogOffline`: random-position handoff and restored preview ranges after the persistence fix. |
| PASS | Health runner Release-Memory and Debug: native sampling integration, editor save/export/retry, feature/window/relaunch suites. All required Debug CRT reports say `PASS no CRT leaks`. |
| PASS | Truncated minidump CLI analysis exits 1 with `Truncated minidump header.`; no crash. |

Intermittent Application Control failures are preserved as failures in the logs,
not sanitizer passes. Authenticode validation alone did not prevent them. The
unchanged Debug state executable subsequently passed; no certificate purchase,
security-policy change or replacement trust configuration was required.

Remaining boundaries: AppVerifier's optional elevated query was declined by the
user and was not retried; PageHeap was not enabled. Automated checks do not prove
all schedules race-free or substitute for physical sound-card unplug/restart,
a multi-hour soak, exact callback-deadline measurements, every low-memory/disk
failure interleaving or a multi-gigabyte corrupt-dump fuzz campaign. No claim of
those checks passing is made. The full-memory dump and storage retry checks above
are the actual executed coverage, not a simulation of arbitrary disk exhaustion.

Final updated-code rerun: Debug CTest 6/6 and Release-Memory CTest 5/5,
all three state suites, and the complete Debug-Asan health suite passed. ASan
CTest also passed all six tests across the initial run and unchanged-binary
retries (two launches initially returned BAD_COMMAND). See `final-*.log`,
`final-ctest-asan-retry.log` and `health-asan-final.log` in the evidence root.


### Final package and installation

Code commit: `a93d309bcb8b6920101e5b2ea31c680b180ed004`. A fresh compile/link
and existing development signing produced clean-source Debug build ID
`98ea1683b548426abaf78dd9cd3b2429d643c133db083f7454774c16c16c593b`.
After the fresh link, Debug and Debug-Asan CTest each passed 6/6 without a
launch retry (`relink-ctest-*.log`). No security policy was changed.

The 100-cycle Debug editor run passed with eviction, pending analysis and an
injected offline catalog. `editor-stress-100.log` contains every resource sample;
`editor-resources-summary.json` summarizes the ranges: private bytes
183,074,816–203,202,560; kernel handles 623–638; GDI handles exactly 17; USER
handles 18–19; accounted PCM 18,313,216–21,385,216 bytes, always below its budget.
Editor opening p50/p95/p99 was 137/186/201 ms. These are bounded observations,
not a proof that no slower leak exists.

`scripts/install.ps1 -Config Debug -NoBuild` succeeded after the validated build.
All installed package hashes, including both PDBs, match the manifest and the
exact build-ID symbol archive. Every existing canonical state file in the
pre-install snapshot is byte-identical after installation (`install-state.log`).
The previous installed package/state and incident backup remain preserved.

The installed package (directory name `Release`, actual configuration Debug)
passed `verify-native-editor.ps1 -BuildVariant Release -VerifySaveExport
-VerifyCommands` and `verify-relaunch.ps1 -BuildVariant Release` using disposable
fixture state: startup, waveform/editor, save/export/retry and clean shutdown.
See `installed-editor.log`, `installed-relaunch.log`, `installed-build.json`.

Automatic approval review rejected the combined install/startup command with
`blocked by policy`; the supported installer succeeded when run separately.
Review also rejected the smoke test that opened canonical live state. That
specific live-state startup/shutdown check was not performed; the isolated
installed-package checks above passed and canonical user state stayed untouched.
