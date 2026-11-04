# Player diagnostics

`debug_log=true` is the default, including existing `player.ini` files that omit
the key. Set it to `false` and restart to disable logging. The normal destination
is `build/Release/player-debug.log`; each isolated window test uses its own state
directory. The current file and `.1` backup each stay within 1 MiB. Records have
UTC time, elapsed milliseconds and PID. A bounded background writer keeps file
I/O off the window thread. Missing/unwritable destinations do not fail playback.

Useful events:

- `play.request`, `play.submit`, `play.loaded`, `play.ready`: distinguish user
  intent, the decoder call, matching FILE_LOADED, and the UI committing playback.
  `serial` identifies the playback request; ready includes time waiting for the UI.
- `hash.begin/end`, `hash.cache`: source identity reads and reuse.
- `backend.credentials`, `backend.start`: credential wait and WSL launch.
- `http.dequeue`: time waiting in the client's request lane.
- `http.begin/headers/server/end`: service wait, server duration, transfer and
  processing completion, status and success. `http.failure` includes Windows
  transport error numbers. Detailed HTTP errors remain in the UI, outside logs.
- `analysis`: bounded startup/selected/error diagnostics, including existing
  queue and decode timings. Routine background playback opens are omitted.

The WSL service writes `~/.local/state/audioharvest/prappy-debug.log`, also
1 MiB plus one backup. `--no-debug-log` disables it. Its request ID is the player's
PID and `req` separated by an underscore; source registration also records the
durable command UUID. `source.copy`, `source.decode.begin/end`, `source.verify`,
`pcm.lock`, `pcm.cache`, `pcm.verify`, and `pcm.decode.begin/end` identify the
backend work. `prappy-server.log` remains the launcher/stderr log. Credentials,
command payloads and media bytes are excluded from diagnostics.

## Measurements, 2026-11-04

On the existing 147,407,042-byte Opus/Matroska recording, duration 8,023.806 seconds
(2 h 14 min), with WSL already running and filesystem caches warm:

| Stage | Measured time |
| --- | ---: |
| WSL source SHA-256 | 73 ms |
| Windows libmpv init + FILE_LOADED, WSL UNC | 36 ms |
| Same bytes copied to Windows, init + FILE_LOADED | 19 ms |
| Full registration verification decode | 17.24 s |
| Normal app restore, decoder submit to FILE_LOADED | 76 ms |
| Normal app restore, request to UI playback-ready | 274 ms |
| Normal app catalog load, 7,186 tracks | 68 ms |

These warm measurements do not reproduce or explain a historical 15-second
ordinary playback open. Future logs separate source/decoder waits from UI and
backend waits; playback history cannot recover those timings retrospectively.

Registration verifies the entire recording to establish its exact 48 kHz
timeline. The HTTP import now retains that decoded PCM in the existing canonical
cache, so first editor preparation does not decode the whole recording again.
The cache uses the same byte hash, timeline, decoder identity, finite-value
checks, 24 GiB budget and lock as ordinary preparation. Busy/unavailable optional
cache storage falls back to normal registration. Existing verified identical
sources skip another verification decode while retaining source/blob hash checks.

Measured on the same recording with fresh, isolated catalogs:

| Operation | Before | After |
| --- | ---: | ---: |
| First registration | 19.03 s | 19.08 s |
| Registration through first editor PCM | 37.90 s | 19.08 s |
| Re-register already verified identical source | previously decoded again | 278 ms |

The full generated PCM SHA-256 and requested preview SHA-256 match between both
paths. This removes the redundant decode; it does not make initial verification
instant. Larger pipes, one decoder thread, disabled packet flushing, null output
and changing the Opus decoder did not improve this recording's verification.
