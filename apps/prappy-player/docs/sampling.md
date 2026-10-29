# Sampling recordings with audioharvest

Prappy Player stays native Windows. The audioharvest HTTP service runs in WSL.
It exposes only explicitly configured library roots and requires a private token.
No library registry, original audio or collection process is changed by connecting.

## Start using it

While listening, **right-click the main waveform**:

- **Open song in sample editor (Ctrl+M)** opens this song and reveals the listening position.
- **Select sample on waveform (Ctrl+E)** shows two edges. Drag a range, adjust either
  edge, or move the band. Click **Open selection in editor (Enter)** when ready.
- **Samples from this song (Ctrl+Alt+M)** opens its existing samples.
- **Cancel selection (Esc)** returns to ordinary seeking. Escape during a drag first
  restores the range from before that drag.

Selecting a passage leaves the song playing. Confirming the handoff pauses it;
editor Play loops that exact range initially. **Edit samples** beside the waveform
and **More > Open current song in sample editor** provide the direct route too.
Compact mode offers the same menu and expands for range selection.
Opening a song alone does not create a saved sample: choose **New sample at cue**
for an explicit four-second draft, or use an existing sample.

**Songs in editor** contains one workspace per canonical source. Its **Samples from
this song** pane combines saved regions and local drafts; overlapping cuts are allowed.
New selections create independent drafts; the same range reopens existing work.
**Save sample** creates or updates one versioned region; **Save as new sample** is an
explicit independent variant. Switching songs and restarting retain each song's
active sample, other drafts, viewport and editing preferences.
Prappy starts the local WSL backend if needed and connects automatically.
**You do not need a terminal, server options, or connection settings.**
The server and token-file fields are prefilled for Workstation.

For standalone backend use, run just this from the scraper checkout:

```bash
./prappy.sh
```

That script takes no options. It uses the existing library at
`~/.local/share/audioharvest/phase-02-library`, the private token at
`~/.config/audioharvest/server.token`, and localhost port 8765. Concurrent launches
share one server. The backend stays available after closing the player; startup
errors are logged in `~/.local/state/audioharvest/prappy-server.log` in WSL.
No models or packages are downloaded. No Windows security settings are changed.
The Connection dialog is available only if you intentionally use another server;
normal local use requires no edits there.
The launcher also exposes the existing artist-search library when present. Ctrl+M
resolves the playing file across all configured libraries and switches to its
registered source automatically; it does not depend on the browser's chosen library.

Choose a library, then a recording, saved sample or collection. Search and metadata
filters use effective recording/sample facts. The browser pages through recordings
and samples. **Ctrl+M** pauses the listening song and resolves its full file hash
across connected libraries, even while the editor is connecting. The position is
captured when you press the shortcut. Unregistered or ambiguous files require explicit
source selection/import; the player never silently invents provenance.

The green In and gold Out handles select canonical 48,000 Hz stereo frames. Out
is exclusive. Zoom and pan the detail plot, double-click the overview to focus,
type exact frame coordinates, or nudge by one frame. One drag is one undo step.
**Zoom to selection (F)** above the waveform fills the horizontal view with the
selected sample and 10% padding on each side (clipped at the recording edges).
It changes only the time view; typing F in a text field does not trigger it.
Snap supports onsets, beats, sixteenth notes and zero crossings; Alt bypasses it.
Audition a selection, its boundaries, or a loop using the canonical float PCM also
used by preparation. Preview gain affects audition independently of song gain.

### Editor playback

The editor has its own transport, and pauses ordinary song playback while open.
**Selection** is the default when choosing a recording or saved sample, with
**Loop enabled initially**. The selected region is shaded green. A white playhead
in both waveforms shows playback position; the status row identifies Playing,
Paused, Loading, Buffering or Stopped, the scope, elapsed/duration and source time.

- **Selection:** play between In and Out. The loop toggle takes effect immediately.
- **Context:** include one second before and after the selected region, clamped
  to the recording. The surrounding playback range is tinted blue.
- **Recording:** play the source from the playhead. Long recordings stream in
  bounded background chunks rather than requiring a whole-song download.

**Play/Pause/Resume** and **Space** control this editor transport. Space does not
interrupt typing. **Stop** resets the playhead to the scope's beginning. Click the
detail waveform to position/seek the playhead within the chosen scope; dragging
time or amplitude views never changes playback boundaries. **Follow playhead**
scrolls when playback moves outside the view; leave it off to inspect freely.

While auditioning Selection or Context, boundary edits apply after releasing the
handle, with an explicit loading state while the revised cut is fetched. Undo/redo
and numeric edits update that audition too. **Hear start/end** are labeled one-shot
boundary checks; they do not inherit looping. Switching recordings stops playback.
Loop, scope and follow preferences persist, but reopening never starts sound itself.

The **Display** row above stereo detail controls waveform amplitude visually:

- **Actual level:** fixed full-scale reference.
- **Fit visible:** fit the visible time range, updating after zooming or panning
  settles. Stereo channels share a scale, preserving their relative levels.
- **Manual:** keep the current scale as you navigate.
- **Fit now:** fit the visible range once and hold that scale in Manual.

Drag the left amplitude ruler **up to enlarge**, **down to shrink**. Dragging switches
to Manual without jumping. Double-click it to reset Actual level. Ruler ticks show
source dBFS; the Display row shows magnification and its dB equivalent. Each channel
stays centered on zero; enlarged drawing is clipped inside its own lane.
**This never changes playback volume, sample boundaries, source PCM or exports.**
Mode and magnification survive closing/reopening. Silence uses unity scale and
enlargement is bounded. The overview keeps its reference scale for context.
Fitting uses exact PCM when it covers the visible range, otherwise conservative
overlapping overview peaks until precise PCM arrives. Envelopes are cached when
the viewport/data changes rather than rescanned every rendered frame.
Fractional zoom/pan bounds request every intersecting sample using the same frame
range as drawing. While detail is loading, a label distinguishes the overview
from precise PCM; views over sixty seconds indicate that zooming reveals detail.

**Classify / detect beats** submits a durable CPU job. Energy, tempo, onsets and
beats are estimates with processor/evidence identities. They never move a cut or
become human acceptance automatically. Correct beat frames/downbeats explicitly
and save the corrected grid. Existing Beat This evidence is displayed when present;
its separately installed model worker remains an explicitly operated backend tool.
No semantic/key/instrument model is downloaded or claimed to have run.

Recording corrections apply to the recording across registration updates. Sample
facts belong to an exact region revision, inherit recording context, and override
only explicit facets. Human corrections outrank imports and machine proposals;
clearing an edited field records explicit rejection. Evidence remains inspectable.

Save creates or advances a sample revision with optimistic concurrency. Collections
pin exact revisions. **Export sample** downloads a SHA-256-verified lossless WavPack
artifact preserving float PCM; the backend retains its source map and preparation.
Export uses the saved revision. Save local edits before exporting those edits.

Drafts and pending command UUIDs persist in `sample-workspaces.json`, together with
metadata context, browser filters/page, viewport, snap, gain and loop. Retrying a
request returns its original committed revision. Conflicts preserve local edits:
reload the saved sample or fork an independent cut. A pending save must be resolved
before another save. Source switches and late replies do not overwrite newer drafts.

Resource limits include a 1 MiB command body, ten-minute PCM requests, 180-second
CPU analysis windows, and a 24 GiB derived PCM cache. Cache corruption is rebuilt;
changed/missing originals are rejected. Original files are never modified.

The normal build is `build/Release/PrappyPlayer.exe`, which is also the existing
pinned shortcut target. Sampling-suffixed variants are isolated test builds;
validated changes must also be rebuilt into the normal Release destination. See [development-signing.md](development-signing.md)
for the measured, intermittent Windows launch behavior; signature validation alone
does not guarantee Smart App Control acceptance.

Validation against an isolated running server:

```powershell
./scripts/verify-sampling.ps1 -Manifest ./build/sampling-integration.json -BuildVariant Release-Sampling
```

The manifest supplies `url`, `token_file` and `library_id`. The GUI test writes
only isolated player state and explicit test samples in that configured library;
use a disposable library, never an existing user's music library.


Playback requests run independently of library browsing and waveform requests.
An expensive browse cannot hold Play in Loading. Catalog browse/search uses one
verified read snapshot per request; unfiltered pages only decorate visible rows.
The Ctrl+M regression (`verify-sampling.ps1 -MarkOnly -Manifest ...`) exercises
cold/warm connection, then presses editor Play and checks SDL-submitted audio
frames and the advancing selection playhead within five seconds. Run it against
an isolated server with delayed recording-list responses to verify independence.


After initial playback starts, background preparation resolves source identity,
loads its sample summaries and warms a bounded eight-second window of verified
cached PCM. Its state is shown in the Edit samples tooltip. It never launches
classification or a speculative whole-song decode; unavailable/busy PCM is labeled
“audio prepared on demand.” Explicit editor requests prepare cold audio normally.
File hash reuse checks Windows file identity, size, write and change times. Warm
requests are fenced on loaded-song generation and do not open or modify drafts.

Source workspaces use library/capture/timeline IDs, not filenames or playlist rows.
Pending sample saves retain their original payload and request ID across source
switches and restart. Late acknowledgments advance the right region while retaining
newer edits. A conflict stays local until explicitly resolved. Corrupt workspace
state is preserved and shown as a recovery error instead of overwritten.
