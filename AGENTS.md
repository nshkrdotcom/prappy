# Prappy development

## Player logs and crash reports

For a player crash, read [the diagnostics guide](apps/prappy-player/docs/diagnostics.md)
first. Do not start with a recursive search of the user profile.

- The normal installed player and its state/log are in
  `apps/prappy-player/build/Release/`. This directory can contain a **Debug**
  installation; check `player-build.json`, not the directory name.
- Start with `apps/prappy-player/build/Release/player-debug.log` and its `.1`
  rotated backup. Log timestamps are UTC; correlate by PID and local crash time.
- Current incidents are in `%LOCALAPPDATA%\Prappy\Reports\incidents`; exact
  packages/PDBs are archived under `Reports\symbols`, and lifecycle records
  under `Reports\sessions`. Reporting works in Debug and Release independently
  of `debug_log`. Normal exits do not create crash incidents.
- From the repository root, run
  `pwsh apps/prappy-player/scripts/inspect-crash.ps1` to analyze the latest
  incident with archived symbols. Expect an original throw/fault source line,
  not just an exit stack. Check `fatal.json`, `event.json`, `session.json` and
  `crash.dmp` when present. The incident ID joins these files.
- The app automatically starts the local-only collector on `127.0.0.1:19876`.
  It requires no cloud account. Do not point diagnostics at an external DSN.
- Preserve relevant evidence before changing packages. Failure retention is
  bounded to 50 incidents/1 GiB (always retaining the newest); normal restarts
  do not displace failures. Legacy ProcDump files remain under
  `%LOCALAPPDATA%\Prappy\CrashDumps`; the old startup-pruning monitor is retired.
- Use `scripts/build.ps1`, then `scripts/test-crash-reporting.ps1 -BuildVariant
  Debug` from the player directory. Test Release in a separate variant such as
  `scripts/build.ps1 -Config Release -VariantSuffix Reporting`, not directly in
  the installed `build/Release` directory. Reporting probes use disposable
  state under `build/reporting-validation` and a separate loopback port (19877).
- Install with `scripts/install.ps1` to verify signatures, package hashes and
  matching symbols while preserving user state. Do not copy only the player
  executable: its reporting helpers are required package files.
