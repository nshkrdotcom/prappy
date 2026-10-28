# Sampling recordings with audioharvest

Prappy Player stays native Windows. The audioharvest HTTP service runs in WSL.
It exposes only explicitly configured library roots and requires a private token.
No library registry, original audio or collection process is changed by connecting.

Start the server in a WSL terminal from the scraper checkout:

```bash
./scripts/serve.sh --root /absolute/path/to/library \
  --token-file "$HOME/.config/audioharvest/server.token"
```

The server creates a private token file if absent and listens on localhost:8765.
Keep this foreground terminal open. Multiple `--root` arguments expose multiple
libraries. Install server dependencies deliberately through the pinned studio
environment; this launch command never installs anything or downloads models.

In Prappy, select **More → Sampling workspace**, then **Connection**. Use
`http://127.0.0.1:8765` and the token-file path, for example
`\\wsl.localhost\ubuntu26-dev\home\home\.config\audioharvest\server.token`.
The worker reads credentials; the draft contains only the path. Remote connections
require HTTPS. HTTP redirects are refused to protect bearer credentials.

Choose a library, then a recording, saved sample or collection. Search and metadata
filters use effective recording/sample facts. The browser pages through recordings
and samples. **Ctrl+M** pauses the listening song and resolves its full file hash
against the chosen library. Unregistered or ambiguous files require explicit
source selection/import; the player never silently invents provenance.

The green In and gold Out handles select canonical 48,000 Hz stereo frames. Out
is exclusive. Zoom and pan the detail plot, double-click the overview to focus,
type exact frame coordinates, or nudge by one frame. One drag is one undo step.
Snap supports onsets, beats, sixteenth notes and zero crossings; Alt bypasses it.
Audition a selection, its boundaries, or a loop using the canonical float PCM also
used by preparation. Preview gain affects audition independently of song gain.

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

Drafts and pending command UUIDs persist in `sample-draft.json`, together with
metadata context, browser filters/page, viewport, snap, gain and loop. Retrying a
request returns its original committed revision. Conflicts preserve local edits:
reload the saved sample or fork an independent cut. A pending save must be resolved
before another save. Source switches and late replies do not overwrite newer drafts.

Resource limits include a 1 MiB command body, ten-minute PCM requests, 180-second
CPU analysis windows, and a 24 GiB derived PCM cache. Cache corruption is rebuilt;
changed/missing originals are rejected. Original files are never modified.

The development build currently lives in `build/Release-Sampling`, separately
from the older pinned `build/Release` executable. See [development-signing.md](development-signing.md)
for the measured, intermittent Windows launch behavior; signature validation alone
does not guarantee Smart App Control acceptance.

Validation against an isolated running server:

```powershell
./scripts/verify-sampling.ps1 -Manifest ./build/sampling-integration.json -BuildVariant Release-Sampling
```

The manifest supplies `url`, `token_file` and `library_id`. The GUI test writes
only isolated player state and explicit test samples in that configured library;
use a disposable library, never an existing user's music library.
