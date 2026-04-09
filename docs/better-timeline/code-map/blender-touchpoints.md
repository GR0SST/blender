# Blender Touchpoints

Use this doc when a Better Timeline change crosses the boundary between the editor folder and core Blender integration points.

## Space Registration

Better Timeline becomes a real Blender editor through these files:

- `source/blender/editors/include/ED_space_api.hh`
- `source/blender/editors/space_api/spacetypes.cc`
- `source/blender/makesdna/DNA_space_enums.h`
- `source/blender/makesrna/intern/rna_space.cc`

The path is:

1. add the space enum
2. define the space struct
3. register the spacetype during editor startup
4. expose the space in RNA so the editor selector can show it

## DNA / RNA Split

Persistent storage lives in DNA. UI-facing and Python-facing access live in RNA.

For Better Timeline that means:

- add persistent fields in `DNA_space_types.h`
- normalize/copy/read/write them in `better_timeline_data.cc`
- expose them in `rna_space.cc` only if UI/Python needs them

Do not skip the RNA setter path for clip range changes. `start_frame`, `end_frame`, and `duration` setters in `rna_space.cc` already route through centralized placement validation via `ed::better_timeline::track_can_place_clip()`.

## Python UI

The Better Timeline header and sidebar live in:

- `scripts/startup/bl_ui/space_better_timeline.py`

That file should stay focused on presentation and UI composition. If validation or state derivation becomes complex, move that logic into C++ RNA/helpers instead of duplicating model rules in Python.

The module is imported from:

- `scripts/startup/bl_ui/__init__.py`

If the module is not imported there, the UI will not appear even if the C++ space exists.

## Animation Integration

Ruler scrubbing depends on shared animation operators and explicit Better Timeline allowlisting.

Most important external file:

- `source/blender/editors/animation/anim_ops.cc`

Key rule:

- `ANIM_OT_change_frame` must allow `SPACE_BETTER_TIMELINE`, otherwise the ruler draws but click/drag scrubbing does nothing

Also keep in mind:

- Better Timeline intentionally does not enable `ED_KEYMAP_ANIMATION` on the window region because that also installs marker polling that currently crashes through `ED_markers_region_visible()`
- Time Scrub and View2D keymaps are manually layered in `better_timeline_ops_view.cc`

## Undo Registration

The custom Better Timeline undo type is registered from:

- `source/blender/editors/undo/undo_system_types.cc`

If undo behavior changes or a new Better Timeline-specific undo requirement appears, check both the implementation in `better_timeline_data.cc` and the global registration point.

## Addon Extensibility

Addon-defined track and clip types are exposed via RNA in:

- `source/blender/makesrna/intern/rna_ui.cc`

Important points:

- `BetterTimelineTrackType` and `BetterTimelineClipType` are runtime-registerable
- `clip_type_poll` is the extensibility hook for custom compatibility logic
- registration ultimately feeds the same central registry used by built-in types

This means custom types should integrate through the registry, not through hardcoded special cases in operators or panels.

## Build Integration

Better Timeline must be wired into the editors build through:

- `source/blender/editors/space_better_timeline/CMakeLists.txt`
- `source/blender/editors/CMakeLists.txt`

If you add a new Better Timeline translation unit and forget the CMake file, the code simply will not build into Blender.

## Notification And Redraw Plumbing

Better Timeline UI updates often depend on standard Blender notifiers rather than bespoke event paths.

Common touchpoints:

- `rna_SpaceBetterTimeline_state_update()` in `rna_space.cc` marks `bmain->is_memfile_undo_flush_needed` and sends `NC_SCREEN | NA_EDITED`
- operator paths usually call `WM_event_add_notifier()` or `ED_area_tag_redraw()`
- header redraw behavior is handled in the header region listener in `space_better_timeline.cc`

If a UI change appears to work in data but not redraw, check notifiers before changing unrelated logic.
