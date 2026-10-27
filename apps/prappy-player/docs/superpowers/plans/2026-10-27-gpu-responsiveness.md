# GPU Responsiveness Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for inline implementation, followed by an independent code review.

**Goal:** Responsive 4K playback and library browsing using hardware rendering without NVIDIA overlay injection on this machine.

**Architecture:** Keep SDL input, ImGui, render resources and presentation on the main thread. Use SDL Direct3D 11 behind a small presentation owner; keep immutable analysis output and asynchronous mpv seeking. Separate shared text layouts from colored/highlighted raster entries and protect the visible working set. Add bounded frame timing and input-to-present diagnostics. Apply the user-authorized, undocumented NVIDIA overlay exclusion only to PrappyPlayer.exe, with a backup and readback.

**Tech Stack:** C++20, SDL3 Direct3D 11, Dear ImGui, DirectWrite, NVAPI driver profiles, PowerShell verification.

**Spec:** User approved GPU rendering, text-cache refactoring, 4K latency measurement and trying per-executable NVIDIA setting 0x809D5F60=0x10000000. No global overlay/notification changes. Preserve existing player state and running process; no compatibility layers.

## Review Focus

- Profile must not attach to another application's or the global driver profile.
- Cached layouts must not leak highlight effects between appearances.
- Renderer reset/resize, Unicode and waveform copies must work on Direct3D 11.
- GPU queue depth must remain bounded; CPU timing must not be presented as photon latency.
- Idle UI must still wait for events; large visible text sets must not churn every frame.

## Steps

- [x] Back up NVIDIA profiles; apply only the per-executable exclusion and verify readback before GPU launches.
- [x] Add a failing large-visible-text/cache regression, then separate text layout and appearance caches.
- [x] Introduce GPU presentation ownership, low-latency pacing and frame diagnostics; verify actual driver name.
- [x] Benchmark compact/4K, expanded list and seek feedback; check injected modules and screenshots.
- [x] Run strict builds, relevant state/UI/window checks and leak checks; review, document, commit and push.


Ruling: Work remains in the existing user-visible workspace, using isolated suffix builds and state directories for verification. The user authorized implementation, driver-profile modification and commit/push; no additional worktree or approval round is required.

Evidence: see VERIFICATION.md GPU rendering and 4K responsiveness section. Renderer reset handling was added following independent review. ASan runtime is blocked by Windows Application Control; Release/Debug and leak checks passed.
