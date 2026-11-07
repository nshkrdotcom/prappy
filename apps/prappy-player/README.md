# Prappy Player

Native Windows x64 player and sampling editor using SDL3, Dear ImGui, ImPlot
and native SDL audio. Indexed source access and analysis run independently in WSL.

The compact player starts at 500 physical pixels wide. It retains geometry,
volume, browsing preferences, listening history and editor drafts. DirectWrite
shaping and font fallback handle Unicode filenames and search. Playback,
HTTP requests and persistence run independently of the window thread.

## Audio architecture

Audioharvest preserves acquired originals in their compressed formats. The reader
seeks and decodes bounded native-frame regions on demand. Prappy shares one
RAM-bounded PCM session between playback and editing; resampling happens at the
device boundary. Saved raw regions are frozen losslessly with effects retained
as metadata. Compact, regenerable peaks supply broad waveform views.

Source hashing and analysis do not gate editor opening. Optional analysis can be
pending or unavailable while indexed playback remains usable. Superseded source,
span and transport requests cannot replace the current view.

## Backend setup

This branch is undergoing validation against an **isolated evaluation root**;
production migration is still pending. The root contains copied canonical state
and read-only original references. Do not redirect it at production journals.
Run the scraper indexed service against that initialized root:

```bash
.venv-studio/bin/python -m audioharvest.indexed_server --evaluation-root "$EVALUATION_ROOT" --native-state
```

Use the root's configured endpoint and private token in Prappy's
`sampling-connection.json`, and launch `PrappyPlayer.exe --evaluation STATE_ROOT`.
The evaluation marker must match the isolated state root and connection. Ordinary
production startup remains a separate cutover task. See
[sampling workflow](docs/sampling.md) for editor controls.

## Build and configuration

Use PowerShell 7, MSVC x64, Windows SDK, CMake and Ninja. Toolchain setup lives
in `dotfiles_private/windows/bootstrap_windows`; set `DOTFILES_PRIVATE` if needed.

```powershell
pwsh scripts/deps.ps1
pwsh scripts/build.ps1 -Run
pwsh scripts/build.ps1 -Config Debug
```

Executable: `build/Release/PrappyPlayer.exe`. Keep `fonts/`, `licenses/` and the
icon with it. The MSVC runtime and SDL are statically linked. Explicit regression
executables and their independent MPV oracle live under `build/Release/tests/`;
they are excluded from the player runtime.
State beside the executable includes `player.ini`, `player-ui.txt`,
`player-history.txt`, `sampling-connection.json` and `sample-workspaces.json`.
Preserve those files and their backups when relocating the application.

Libraries are configured in WSL. Player settings are:

```ini
history_limit=10000
add_new_listening_history_entry_when_replaying_history_item=false
debug_log=true
normalization_target_lufs=-18
normalization_unmeasured_fallback_db=0
```

Unknown or malformed settings produce an error. History retention accepts
1–1,000,000 visits; normalization target -36 to -5 LUFS; fallback -24 to +12 dB.
Normalization enable and volume are saved in `player-ui.txt`.

## Controls and playback

- **List** opens the library; **History** shows successful visits. Search supports
  quoted phrases and independent words. **Ctrl+F** focuses it; **Escape** clears it.
- **Space** toggles playback. Back/Forward follow the listening trail. Queue,
  Play Next, folder scope and repeat control progression independently of search.
- History-row replay reuses its visit by default; the optional setting above
  creates a new visit. Failed and superseded loads add no visit.
- Filters cover codec, availability, duration and listening-history views.
  Library refresh preserves the loaded source and gain decision.
- Drag the waveform to seek; focused controls support Left/Right, Home and End.
  The divider above the title changes waveform height.
- **More** contains loudness matching, topmost and slim strip mode.
  **Ctrl+Shift+T** toggles topmost; **Ctrl+Shift+M** toggles strip mode.
- Right-click a recording for original-location copy/reveal actions.

Loudness matching chooses valid original ReplayGain/R128 tags, then the prepared
listening measurement, then fallback. Gain freezes for each acknowledged
playback; seek, pause and editor work retain it. User volume is independent.
Boost is limited to +12 dB and a -1 dB sample-peak ceiling when a trusted peak
is available. Unknown peaks cannot authorize boost. This is not true-peak
limiting. Mandatory Opus header gain is included during preparation and is not
added again. Sound Check conversion is unsupported. Original tags stay intact.

## Sampling

**Ctrl+M** opens the playing recording at its listening position. Right-click
the waveform to select a range or browse its samples. **Ctrl+E** starts range
selection; **Enter** confirms and **Escape** cancels.

The editor retains native frame coordinates, rate, channels and precision.
Audition uses the explicit prepared listening render with frame conversion.
Saved regions pin their native timeline. Drafts, pending saves, viewport and
cursor survive restart. See [sampling workflow](docs/sampling.md).

## Diagnostics and verification

Compact diagnostics default to on. `player-debug.log` rotates at 1 MiB with
one backup. It records playback/preparation waits, HTTP queue/header/total times,
request identity, cancellation and errors. Credentials and request bodies are
excluded. See [diagnostics](docs/diagnostics.md).

```powershell
ctest --test-dir build/Release --output-on-failure
pwsh scripts/test-state.ps1
pwsh scripts/test-state.ps1 -BuildVariant Debug
pwsh scripts/verify-native-editor.ps1 -Manifest C:\absolute\fixture.json
```

The state suite creates two short audio fixtures. Native backend/UI tests use
isolated WSL libraries from `tests/create_cold_start_fixture.py`. The editor
verifier supports `-Iterations 100`, `-EvictSourcePages`, `-InjectAnalysisPending`
and `-InjectCatalogOffline`. Eviction is advisory, not a cold-disk guarantee.
`scripts/test-output-meter.ps1` is an explicit audible WASAPI test.

Historical implementation/verification reports retain earlier measurements.
Current architecture, measured corpus sizing and the complete migration plan
are in `~/jb/docs/20261104/prappy_scraper/` on the workstation.
See [THIRD_PARTY.md](THIRD_PARTY.md) for dependencies and licenses.
