# Removal audit, 2026-10-29

The user's existing commit is `7456762199287052b79e8a2fff90c8615276ba2d`.
It was reviewed in place, without reapplying an archive or recreating that commit.
This report was absent from that checkout and is added as a follow-up.

`git diff 7456762^ 7456762 --name-status --diff-filter=DR` returns no entries.
No tracked file was deleted or renamed. The repository trees contain 1,650
tracked files before and 1,661 after that commit. Those repository counts are
not an independent audit of the supplied archive's reported 1,585 originals.

The removals are symbols and execution paths:

| Removed symbol/path | Current owner |
| --- | --- |
| `analysis::Worker` | `analysis::Service`, bounded `analysis_queue::Queue` |
| `approachGain` | Per-loaded-serial `Playback::gainDecision`; no gain ramp |
| `normalizationGain` in `browse.h` | `gain::choose`, called by playback/settings actions |
| `analysis::cachePath` | v2 registry artifact names and independent waveform IO |
| Old cache enumeration/read path | `player-analysis-v2/summaries.bin` plus selected blob reads |

Searches across `src/` and `scripts/` find none of the removed symbols or old
`player-analysis/` runtime readers. The performance scripts now use the v2
namespace too. `analysis::parse` still validates the waveform serialization
format; this is used only for v2-referenced blobs, not old cache discovery.

The initial commit added `analysis_dsp.*`, `analysis_queue.*`,
`analysis_registry.*`, `analysis_service.*`, `gain_policy.*`, and
`tests/portable_analysis_tests.cpp`. Follow-ups add Windows fixture validation,
the fixture generator and these reports. Existing `player-analysis/` data is
not migrated, removed or changed by this work.
