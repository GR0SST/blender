# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a fork of `blender-v5.1-release` that adds a **Better Timeline** editor — a clip-oriented custom editor type implemented in C++ as a first-class Blender space, alongside a Python UI layer. It is not a Python add-on; it is compiled into Blender itself.

`AGENTS.md` in the root contains detailed architecture notes, design constraints, and rules that must be treated as foundational. Read it before making architectural decisions.

## Build & Run Commands

All scripts default `BUILD_DIR` to `../build_darwin` (sibling of the repo root). Override via `export BUILD_DIR=/path/to/build`.

```bash
./dev-build.sh              # ninja -C $BUILD_DIR blender
./dev-install.sh            # ninja -C $BUILD_DIR install
./dev-run.sh                # runs Blender.app with --factory-startup
./dev-build-install-run.sh  # all three sequentially
```

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
- Built-in types: `test_track`→`test_clip`, `animation_track`→`animation_clip`, `spline_track`→`spline_clip`.

### Compatibility & Registry
- All compatibility checks (create, paste, duplicate, move, drag/drop) go through the centralized API in `ED_better_timeline.hh`. Do not scatter `if (track_type == "...")` logic across operator code.
- `BetterTimelineTrackType::clip_type_poll` is the extension point for custom compatibility callbacks.
- Creation menus and drag/drop feedback must read from registered types, not hardcoded lists.

### Operators & Undo
- All data-affecting operators use `OPTYPE_UNDO`. `bScreen` uses `IDTYPE_FLAGS_NO_MEMFILE_UNDO` in Blender 5.1, so track/clip changes will not participate in global memfile undo without a custom `UndoType`.
- `Properties Pane` inline edits (`Start`, `End`, `Duration`) must go through the same centralized placement/compatibility validation path as other clip moves — not direct DNA writes.

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
- Runtime state must be created during space init/duplicate/read and freed from the space lifecycle — do not intentionally leak static storage.

### UI Terminology
- **Track List Pane**: left-side pane with track names and controls
- **Timeline Canvas**: central pane with ruler, playhead, grid, clips
- **Properties Pane**: right sidebar (`N` key), uses `RGN_TYPE_UI` — do not reintroduce custom-drawn sidebar content in the window region
- **Splitter**: resizable divider between Track List Pane and Timeline Canvas; width stored in `SpaceBetterTimeline::track_panel_width`
