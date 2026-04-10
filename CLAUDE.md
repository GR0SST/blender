# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This repository has one purpose: developing the **Better Timeline** editor for Blender.

Better Timeline is a clip-oriented, track-based timeline editor built as a first-class Blender space (C++ + Python, compiled in — not an add-on). It is heavily inspired by **Unity's Timeline** and aims to match its look, feel, and workflow as closely as makes sense within Blender's architecture. Unity's Timeline is the primary design reference for layout decisions, UX patterns, and feature priorities.

The fork base is `blender-v5.1-release`. Everything outside the Better Timeline subsystem is unchanged and should not be modified.

`AGENTS.md` in the root contains detailed architecture notes, design constraints, and rules that must be treated as foundational. Read it before making architectural decisions.

## Build & Run Commands

All scripts default `BUILD_DIR` to `../build_darwin` (sibling of the repo root). Override via `export BUILD_DIR=/path/to/build`.

```bash
./dev-build.sh     # ninja -C $BUILD_DIR blender   ← use this to compile
./dev-install.sh   # ninja -C $BUILD_DIR install   ← use this to install after build
./dev-run.sh       # runs Blender.app with --factory-startup
```

> **IMPORTANT**: Do **NOT** use `./dev-build-install-run.sh`. That script is reserved for the developer's own workflow. Always run `dev-build.sh` and `dev-install.sh` separately, then let the developer run Blender manually (or use `dev-run.sh` only if explicitly asked).

The build system is CMake + Ninja. First-time CMake configuration must be done manually (see Blender's official build docs). There is no single-test runner; use `ctest` in the build directory for automated tests.

## Primary Files for the Better Timeline Editor

**C++ implementation** (`source/blender/editors/space_better_timeline/`):
- `space_better_timeline.cc` — editor/space registration, region setup, lifecycle hooks, keymap wiring
- `better_timeline_data.cc` — DNA persistence, track/clip alloc/free/duplicate, blend file I/O
- `better_timeline_ops_tracks.cc` — track operators (add, delete, select, rename)
- `better_timeline_ops_clips.cc` — clip operators (add, delete, move, copy/paste, duplicate)
- `better_timeline_ops_view.cc` — view operators (zoom, pan, scroll, frame-all, splitter resize)
- `better_timeline_draw.cc` — all rendering: track list, ruler, grid, playhead, clips, overlays
- `better_timeline_layout.cc` — layout/geometry computation for regions, rows, scrollbar
- `better_timeline_types.cc` — runtime type registry for `TrackType` and `ClipType`
- `better_timeline_intern.hh` — internal declarations shared across sibling files
- `source/blender/editors/include/ED_better_timeline.hh` — public API for type lookup, registration, compatibility

**DNA/RNA** (touch these when adding new persistent fields or RNA-exposed properties):
- `source/blender/makesdna/DNA_space_types.h` — `SpaceBetterTimeline`, `BetterTimelineTrack`, `BetterTimelineClip` structs
- `source/blender/makesdna/DNA_space_enums.h` — `SPACE_BETTER_TIMELINE` enum
- `source/blender/makesrna/intern/rna_space.cc` — editor selector menu, active selection RNA
- `source/blender/makesrna/intern/rna_ui.cc` — addon-facing `BetterTimelineTrackType` / `BetterTimelineClipType` RNA registration

**Blender integration** (rarely touched):
- `source/blender/editors/space_api/spacetypes.cc` — global editor-type registration
- `source/blender/editors/animation/anim_ops.cc` — `ANIM_OT_change_frame` must explicitly allow `SPACE_BETTER_TIMELINE` for ruler scrubbing

**Python UI**:
- `scripts/startup/bl_ui/space_better_timeline.py` — header, panels, playback controls, Properties Pane
- `scripts/startup/bl_ui/__init__.py` — loads the module on startup

## Architecture & Key Rules

### Data Model
- `SpaceBetterTimeline` owns `tracks` (a DNA `ListBase`). Each `BetterTimelineTrack` owns `clips` (another `ListBase`). This nested ownership is authoritative — do not redesign clip storage as a detached global list.
- Tracks and clips are **typed**: `track_type` and `clip_type` are persistent string idnames (max 64 chars) resolved against the runtime registry.
- Type-specific data lives in `BetterTimelineTrack::properties` / `BetterTimelineClip::properties` (`IDProperty` blobs), not hardcoded struct fields.
- `BetterTimelineTrack::object` is a weak scene-object binding for object-slot tracks. Treat it like editor UI state, not an owning/refcounted object link.
- Built-in types: `test_track`→`test_clip`, `animation_track`→`animation_clip`, `spline_track`→`spline_clip`.

### Compatibility & Registry
- All compatibility checks (create, paste, duplicate, move, drag/drop) go through the centralized API in `ED_better_timeline.hh`. Do not scatter `if (track_type == "...")` logic across operator code.
- `BetterTimelineTrackType::clip_type_poll` is the extension point for custom compatibility callbacks.
- Creation menus and drag/drop feedback must read from registered types, not hardcoded lists.

### Operators & Undo
- All data-affecting operators use `OPTYPE_UNDO`. `bScreen` uses `IDTYPE_FLAGS_NO_MEMFILE_UNDO` in Blender 5.1, so track/clip changes will not participate in global memfile undo without a custom `UndoType`.
- `Properties Pane` inline edits (`Start`, `End`, `Duration`) must go through the same centralized placement/compatibility validation path as other clip moves — not direct DNA writes.
- Better Timeline custom undo must only participate for explicit Better Timeline state changes. Do not leave `BKE_undosys_step_push_init()` half-open on helper/UI paths that are not real Better Timeline edits.

### Drawing & View2D
- Do **not** enable `V2D_KEEPZOOM` on the Better Timeline `View2D`.
- Do **not** reset `region->v2d.cur` to `tot` every draw — this breaks wheel/MMB navigation.
- `ED_time_scrub_draw()` must be clipped to the timeline rect (right of the track panel) or the ruler bleeds under the left panel.
- Do **not** enable `ED_KEYMAP_ANIMATION` on the window region — it installs marker polling that currently crashes in `ED_markers_region_visible()`.
- Ruler scrubbing works via the standard `ANIM_OT_change_frame` operator; playhead snapping is intentionally disabled until Better Timeline has its own snap/keylist logic.
- During splitter drag, refresh `region->v2d.oldwinx/oldwiny` against the timeline mask or Blender will auto-zoom on every redraw.

### Code Organization
- Keep `space_better_timeline.cc` focused on editor registration, region setup, and top-level lifecycle/glue only.
- New features go into focused sibling `.cc` files (`*_ops.cc`, `*_draw.cc`, `*_utils.cc`), following the existing split. Do not grow the main file.
- `SpaceBetterTimeline::runtime` owns all transient/interaction state. New temporary draw or interaction state goes there, not in static/global objects.
- Runtime also owns Better Timeline-specific undo gating (`undo_push_pending`). If undo behavior changes, update the runtime lifecycle and custom undo poll together.
- Runtime state must be created during space init/duplicate/read and freed from the space lifecycle — do not intentionally leak static storage.

### UI Terminology
- **Track List Pane**: left-side pane with track names and controls
- **Timeline Canvas**: central pane with ruler, playhead, grid, clips
- **Properties Pane**: right sidebar (`N` key), uses `RGN_TYPE_UI` — do not reintroduce custom-drawn sidebar content in the window region
- **Splitter**: resizable divider between Track List Pane and Timeline Canvas; width stored in `SpaceBetterTimeline::track_panel_width`

## Project Docs

The canonical documentation index is `docs/README.md`. Use it to find the narrowest topic doc before starting feature work.

Recommended read order:

1. `AGENTS.md` for foundational Better Timeline constraints and architecture rules.
2. `docs/README.md` for the documentation map.
3. The specific Better Timeline topic doc for the subsystem you are changing.

Do not update Better Timeline docs as part of an implementation attempt until the developer has
explicitly confirmed the change works in their runtime build. If a fix is still unverified or the
developer reports it is still broken, keep the notes out of `docs/` and avoid documenting the
attempt as if it were settled behavior.

When a session reveals something non-obvious — an API quirk, a namespace issue, a constraint, a design decision — add it to the appropriate doc before finishing, but only after the developer has confirmed the implemented behavior actually works.
