# Persistence, Runtime, And Undo

Use this doc before changing any persistent Better Timeline field, temporary interaction state, duplication code, or undo behavior.

## Persistent DNA State

The persistent space and item structs live in `source/blender/makesdna/DNA_space_types.h`.

Important `SpaceBetterTimeline` fields:

- `tracks`
- `selected_track_index`
- `selected_clip_index`
- `next_track_name_index`
- `track_panel_width`
- `track_scroll_offset`

Important item payload fields:

- `BetterTimelineTrack::properties`
- `BetterTimelineClip::properties`

These `IDProperty` roots are the extension points for type-specific data.

## Runtime-Only State

`SpaceBetterTimeline::runtime` is a raw pointer to `SpaceBetterTimeline_Runtime`, declared in `DNA_space_types.h` and defined in `better_timeline_intern.hh`.

Current runtime visual/interaction state includes:

- `track_drag_visual_state`
- `clip_drag_visual_state`
- `clip_box_select_visual_state`
- `clip_resize_visual_state`

Rules:

- runtime state must be created in space init/duplicate/read paths
- runtime state must be freed from the space lifecycle
- do not move transient interaction data into static globals just to avoid lifecycle work

## Lifecycle Hooks

Main ownership flow lives in `source/blender/editors/space_better_timeline/better_timeline_data.cc`.

- `better_timeline_space_state_init()` initializes persistent defaults and ensures runtime storage
- `better_timeline_space_runtime_free()` frees runtime state
- `better_timeline_tracks_free()` / `better_timeline_track_free()` release nested data
- `better_timeline_track_duplicate()` and `better_timeline_clip_duplicate()` deep-copy nested lists and `IDProperty` payloads
- `better_timeline_space_blend_read_data()` rebuilds nested track/clip/property data from `.blend`
- `better_timeline_space_blend_write()` writes the nested lists explicitly

## Deep-Copy Rules

Do not use shallow list duplication for Better Timeline track state.

Required behavior:

- duplicate tracks one-by-one
- duplicate each track's clip list
- duplicate `track->properties`
- duplicate `clip->properties`
- re-run type normalization on restored/duplicated data

This is already handled in `better_timeline_data.cc`; preserve that pattern.

## Post-Read Normalization

`better_timeline_state_normalize_after_read()` repairs state after blend read:

- active indices are corrected if the stored active item is no longer selected
- `next_track_name_index` is clamped to at least `1`
- clip and track type ids are normalized through `better_timeline_track_ensure_type()` / `better_timeline_clip_ensure_type()`

Do not assume raw stored data is immediately valid without this normalization pass.

## Undo Model

Better Timeline has its own custom undo type in `better_timeline_data.cc` and it is registered from `source/blender/editors/undo/undo_system_types.cc`.

Why it exists:

- Better Timeline state lives on `bScreen`
- in Blender 5.1, `bScreen` uses `IDTYPE_FLAGS_NO_MEMFILE_UNDO`
- plain global memfile undo does not restore track/clip editor state correctly by itself

Important implementation details:

- `better_timeline_undo_push_init()` is called before state changes
- undo snapshots deep-copy the full track tree into `BetterTimelineUndoState`
- `use_memfile_step = true` is set so Better Timeline steps stay ordered correctly relative to global undo steps
- decode restores the full snapshot and clears drag visuals

Rules:

- state-changing Better Timeline operators should keep using `OPTYPE_UNDO`
- if you add new persistent editor state, make sure it is included in the undo snapshot/restore path
- if you add new transient runtime state, make sure undo restore clears or rebuilds it appropriately

## Clipboard Note

Track and clip clipboards are not persistent editor state.

- track clipboard currently lives as a static `ListBase` in `better_timeline_ops_tracks.cc`
- clip clipboard currently lives as a static `ListBase` plus anchor frame in `better_timeline_ops_clips.cc`

Treat clipboard as session-only helper state, not as `.blend` data.
