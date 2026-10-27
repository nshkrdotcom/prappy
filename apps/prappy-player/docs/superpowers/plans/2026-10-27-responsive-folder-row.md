# Responsive Folder Row Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement the checked steps below.

**Goal:** Keep directory controls readable at minimum width with active filters first and explicit overflow.

**Architecture:** A pure, lazy compact-row planner allocates bounded chip widths and reserves overflow space. Directory-specific ordering stays in the directory model; the existing browser also serves an overflow overview. DirectWrite performs shaped character trimming with a native ellipsis.

**Tech Stack:** C++20, ImGui/SDL software rendering, DirectWrite, existing PowerShell UI probes.

**Spec:** User-approved behavior: keep Folders and its active count visible, show only readable chips that fit, ellipsize long names, expose remaining choices through +N more, and expand only on explicit interaction. Prioritize includes and exclusions before pinned suggestions. The bounded overflow picker reuses the existing multi-row directory browser rather than automatically growing the toolbar.

## Global Constraints

- Preserve the 500px minimum content width and existing directory/filter persistence semantics.
- No new dependencies, schema changes, or compatibility layers.
- Never measure or render every directory on each frame.
- Keep the reusable planner independent of directories and ImGui.

## Review Focus

- Extremely long Unicode/emoji folder names must trim through DirectWrite without broken clusters.
- Active rules outside the current catalog must remain accessible and removable.
- Resizing must not move controls outside the row or mutate filters.
- Overflow navigation must preserve the original tree browser and stable folder IDs.
- A large directory list must still require only viewport-sized text measurement.

## Steps

- [x] Add failing bounds, overflow reservation, and lazy-measurement tests in `src/feature_tests.h` for `src/compact_row.h`.
- [x] Implement the pure compact-row planner and priority-choice collection in `src/directory_filter.h`.
- [x] Extend `src/text.h/.cpp` and `src/feature_ui.h` with native ellipsis rendering and trimming checks in `src/tests.cpp`.
- [x] Integrate in `src/directory_ui.h`, reusing its clipped table for overflow choices and retaining tree navigation.
- [x] Check minimum/wide widths, overflow actions, active-first ordering, hidden-rule removal, Unicode labels and existing interactions with `scripts/verify-features.ps1` and the state suite.
- [x] Review, update documentation/screenshots as appropriate, commit and push.
