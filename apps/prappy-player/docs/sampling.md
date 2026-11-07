# Sampling indexed recordings

Prappy stays native Windows; audioharvest serves explicit libraries from WSL.
Compressed originals stay intact; bounded native regions supply playback/editing.
This branch is validated in isolated state; production migration is pending.

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

Every source carries library/capture, original-byte and immutable native-clock
identities. Opening the editor adopts the player's retained PCM or seeks directly
to a bounded region. It does not register, hash or decode the whole song.
Optional waveform analysis may still be queued. A newer source/range cancels the
superseded handoff; an unavailable source retains its latest playback intent.

## Native editing and listening

In/Out boundaries are half-open native frames: In is included, Out excluded.
The source's sample rate determines seconds. Mono, stereo and multichannel
sessions retain every channel; integer32 and float64 are not silently quantized.
Waveforms use compact native min/max bins, refined for the visible viewport.

Playback and audition share the native frame clock and retained PCM. One
continuous device-boundary adapter handles resampling, declared channel layout
and listening gain. Saved regions freeze authoritative pre-effect native bits;
they are never reconstructed from the lossy original.

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
blocks and verify sample bits before publication. Project rendering uses
preserved frozen clips, explicit output adapters and native source maps. Current
new saves require the selection to fit retained PCM; larger-than-RAM save streaming
is still being implemented.

## Backend and analysis

Use an initialized isolated evaluation root with copied canonical state and
read-only original references. Configure its URL/token in
`sampling-connection.json`; keep the private token in its own file. The player
and root's evaluation marker must agree. See [backend setup](../README.md#backend-setup).

The independent native analysis worker streams and discards audio, publishing
small regenerable peaks through durable fenced jobs. Playback/editor access does
not wait for library-wide analysis. Classifiers use explicit bounded input
adapters; there is no persistent library-wide listening render. Current trial
service deployment and final production migration are tracked in the implementation
checklist rather than the retired preparation-service instructions.

Source acquisition/import is explicit background/operator work. Editor opening
never imports an original. Import authorization checks the opened Linux descriptor
against configured source roots before reading bytes.

## Persistence, saves and errors

`sample-workspaces.json` beside the executable retains drafts, native references,
viewport/cursor, pending saves and handoffs. Checked atomic writes preserve the
prior file and backup on failure. Original audio is not stored in this file.

Commands have stable request IDs. Repeated acknowledgments do not duplicate
saves. Revision conflicts retain local changes; reload or fork explicitly.
Switching sources or editing while a request runs cannot apply its acknowledgment
to a different draft. Human metadata corrections remain scoped to their target;
recording-level and sample-level facts are distinct.

Compact logging defaults to on: `player-debug.log` records source waits,
HTTP timing and bounded request IDs without credentials/payloads. WSL HTTP logs
for the current isolated services live in the evaluation root's `logs/` directory.

## Isolated verification

`tests/create_cold_start_fixture.py --port PORT --manifest PATH` runs through the
scraper's pinned studio Python and creates a disposable library/server. Optional
rate/channel/subtype arguments cover native precision; `--codec opus` or
`--codec aac` generates compressed-source fixtures. `--source` imports a
read-only copy of a chosen original into that isolated fixture.

```powershell
pwsh scripts/verify-native-editor.ps1 -Manifest C:\absolute\fixture.json
pwsh scripts/verify-native-editor.ps1 -Manifest C:\absolute\fixture.json -InjectAnalysisPending
pwsh scripts/verify-native-editor.ps1 -Manifest C:\absolute\fixture.json -InjectCatalogOffline
```

The verifier checks native opening, audition prefetch and restart. Use
`-Iterations 100` for random-position measurements. `-EvictSourcePages` requests
advisory page eviction of that fixture's encoded source copy. Analysis-pending
injection must leave editing usable; catalog-offline injection verifies recovery.
