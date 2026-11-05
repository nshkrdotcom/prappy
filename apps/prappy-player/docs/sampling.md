# Sampling prepared recordings

Prappy stays native Windows; audioharvest serves explicit libraries from WSL.
Original recordings, permanent working masters and listening renders are separate.

## Open and select

While listening, right-click the main waveform:

- **Open song in sample editor (Ctrl+M)** opens the loaded source at its cue.
- **Select sample on waveform (Ctrl+E)** shows two range edges. Drag the edges
  or move the band, then **Enter** to open it.
- **Samples from this song (Ctrl+Alt+M)** opens its saved samples and drafts.
- **Escape** cancels selection; during a drag it first restores the prior range.

Confirming a handoff pauses listening. Editor playback initially loops the
selected range. Opening a source alone does not save a sample. Use **New sample
at cue**, or choose a saved sample. Return to player preserves editor work.

Every source carries its library/capture IDs, prepared asset and immutable
native timeline. Opening a Ready source reads metadata, stored peaks and short
native sample ranges. It does not register, hash or decode the original song.
A temporary not-ready response retains the cue/range and retries. Returning
or choosing a newer source cancels the superseded handoff.

## Native editing and listening

In/Out boundaries are half-open native frames: In is included, Out excluded.
The source's sample rate determines seconds. Mono, stereo and multichannel
masters retain every channel; integer32 and float64 are not silently quantized.
Wide views read stored min/max levels. Fine detail reads bounded typed samples.

Audition plays the separate 48-kHz stereo float32 listening domain, converting
positions explicitly to/from native frames. Source identity and playback gain
remain pinned while library preparation creates newer profiles. A saved region
continues to identify its exact native timeline.

The editor provides zoom, precise boundaries, undo/redo, beat/onset snapping,
loop audition, metadata corrections and verified lossless export. Machine
classifications, timing anchors and suggested regions remain proposals.

**Songs in editor** retains a workspace per source. Saved regions and local
drafts can overlap. New selections create independent drafts; reopening the
same range finds existing work. **Save sample** updates one versioned region;
**Save as new sample** creates an independent cut. Each source retains its
active draft, cursor, viewport and editing preferences across restart.

Native exports preserve source rate, channel count and supported precision:
WavPack for float32/integer32, Wave64 for float64. Default exports stream bounded
blocks and verify sample bits before publication. Project rendering explicitly
uses the prepared 48-kHz stereo domain and retains native source maps.

## Backend and preparation

In the scraper checkout, `~/.config/audioharvest/workstation.json` names explicit
`libraries` and `source_roots` arrays of absolute paths. The HTTP service and
independent preparer share it. Set `working_root` in
`~/.config/audioharvest/storage.json` to an adequately sized permanent volume.

```bash
./scripts/install-preparation-service.sh
./prappy.sh
```

The user preparation service keeps working after HTTP/player shutdown and
restarts after failure. It observes the configured libraries, persists jobs,
checks original identity, prepares/verifies audio and peaks, then publishes Ready.
Jobs reserve working space before encoding. CapacityBlocked and Offline are
explicit states and recover automatically when their resources return. The
default free-space floor is 20 GiB, configurable as `reserve_gib` in storage.json.
Complete-corpus readiness requires sufficient storage and
successful preparation of every required source.

The local player connects automatically to localhost:8765 using the existing
private token file. A custom server uses `sampling-connection.json` beside the
executable, containing `url`, `token_file` and `library`. Credentials remain in
the private token file. Standalone explicit-root serving remains available:

```bash
./scripts/serve.sh --root /absolute/library --source-root /absolute/originals \
  --working-root /absolute/working --token-file /absolute/private-token
```

Source acquisition/import is explicit background/operator work. Editor opening
never starts an original-file import. Import authorization checks the opened
Linux descriptor against configured source roots before reading bytes.

## Persistence, saves and errors

`sample-workspaces.json` beside the executable retains drafts, native references,
viewport/cursor, pending saves and handoffs. Checked atomic writes preserve the
prior file and backup on failure. Original audio is not stored in this file.

Commands have stable request IDs. Repeated acknowledgments do not duplicate
saves. Revision conflicts retain local changes; reload or fork explicitly.
Switching sources or editing while a request runs cannot apply its acknowledgment
to a different draft. Human metadata corrections remain scoped to their target;
recording-level and sample-level facts are distinct.

Compact logging defaults to on: `player-debug.log` records preparation waits,
HTTP timing and bounded request IDs without credentials/payloads. WSL HTTP logs
are under `~/.local/state/audioharvest/`; preparation logs use
`journalctl --user -u audioharvest-preparation.service`.

## Isolated verification

`tests/create_cold_start_fixture.py --port PORT --manifest PATH` runs through the
scraper's pinned studio Python and creates a disposable library/server. Optional
rate/channel/subtype arguments cover native precision. `--source` imports a
read-only copy of a chosen original into that isolated fixture.

```powershell
pwsh scripts/verify-prepared-editor.ps1 -Manifest C:\absolute\fixture.json
pwsh scripts/verify-prepared-editor.ps1 -Manifest C:\absolute\fixture.json -InjectNotReady
pwsh scripts/verify-prepared-editor.ps1 -Manifest C:\absolute\fixture.json -InjectCatalogNotReady
```

The verifier checks native opening, audition prefetch and restart. Use
`-Iterations 100` for random-position measurements. `-EvictPreparedPages`
requests advisory page eviction of that fixture's converted audio only.
