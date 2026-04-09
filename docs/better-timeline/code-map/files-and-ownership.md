# Files And Ownership

Use this as the fast file map before grepping through the whole Blender tree.

## Core Better Timeline Folder

All primary editor implementation files live in `source/blender/editors/space_better_timeline/`.

| File | Primary responsibility |
|---|---|
| `space_better_timeline.cc` | space bootstrap, region registration, lifecycle glue, main-region cursor callback |
| `better_timeline_intern.hh` | shared internal declarations and helper contracts across Better Timeline modules |
| `better_timeline_data.cc` | track/clip allocation, free, deep-copy, selection helpers, normalization, blend read/write, custom undo type |
| `better_timeline_layout.cc` | pane geometry, rect helpers, row/button/clip hit-testing, scrollbar math |
| `better_timeline_draw.cc` | draw passes for track list, canvas, overlays, clips, playhead, scrollbars |
| `better_timeline_ops_view.cc` | scroll, pan/zoom integration, splitter resize, frame-all, Better Timeline keymap wiring |
| `better_timeline_ops_tracks.cc` | track selection, add/delete/reorder, mute/lock behavior, add-track menu, track clipboard |
| `better_timeline_ops_clips.cc` | clip selection, drag, resize, box select, create/delete/duplicate, copy/paste, clip clipboard |
| `better_timeline_types.cc` | runtime registry for built-in and addon-defined track/clip types plus compatibility logic |
| `CMakeLists.txt` | compilation unit list for the Better Timeline editor library |

## Public Editor API

These are the Better Timeline APIs meant to be used from outside the editor folder:

- `source/blender/editors/include/ED_better_timeline.hh`

Use this for:

- track/clip type lookup
- compatibility queries
- overlap rules
- registration of addon-defined types

Do not reach into `better_timeline_types.cc` internals directly from unrelated code.

## Persistent Data Definitions

| File | Responsibility |
|---|---|
| `source/blender/makesdna/DNA_space_enums.h` | `SPACE_BETTER_TIMELINE` enum |
| `source/blender/makesdna/DNA_space_types.h` | `SpaceBetterTimeline`, `BetterTimelineTrack`, `BetterTimelineClip`, flags, runtime pointer |

If a change must survive save/load or undo snapshots, it usually touches these files and `better_timeline_data.cc`.

## RNA And UI Surface

| File | Responsibility |
|---|---|
| `source/blender/makesrna/intern/rna_space.cc` | space selector entry, `SpaceBetterTimeline`, `BetterTimelineTrack`, `BetterTimelineClip` RNA, active selection accessors, clip range validation setters |
| `source/blender/makesrna/intern/rna_ui.cc` | addon-facing runtime registration for `BetterTimelineTrackType` and `BetterTimelineClipType` |
| `scripts/startup/bl_ui/space_better_timeline.py` | header menus, playback controls, Properties Pane panels |
| `scripts/startup/bl_ui/__init__.py` | imports the Better Timeline Python UI module at startup |

## Blender Integration Files Outside The Submodule

| File | Why it matters |
|---|---|
| `source/blender/editors/space_api/spacetypes.cc` | registers Better Timeline as a real space during startup |
| `source/blender/editors/include/ED_space_api.hh` | declares the Better Timeline spacetype init function |
| `source/blender/editors/animation/anim_ops.cc` | Better Timeline must be explicitly allowed in `ANIM_OT_change_frame` for ruler scrubbing |
| `source/blender/editors/undo/undo_system_types.cc` | registers the custom Better Timeline undo type |
| `source/blender/editors/CMakeLists.txt` | makes the editor submodule part of the editors build |

## Ownership Heuristics

- If a task is mostly space creation or region wiring, start in `space_better_timeline.cc`.
- If a task needs rect math or hit-testing, start in `better_timeline_layout.cc`.
- If a task changes visuals but not layout math, start in `better_timeline_draw.cc`.
- If a task changes gestures or hotkeys, start in `better_timeline_ops_view.cc` plus the relevant track/clip ops file.
- If a task changes persistence or nested ownership, start in `better_timeline_data.cc`.
- If a task changes type compatibility, start in `ED_better_timeline.hh` and `better_timeline_types.cc`.

## Architectural Rule

Do not collapse new work back into `space_better_timeline.cc`. If a feature grows beyond glue-level code, create or extend a focused sibling module.
