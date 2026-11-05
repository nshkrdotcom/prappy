# Player diagnostics

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
