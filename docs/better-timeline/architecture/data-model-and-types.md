# Data Model, Types & Visual States

> Reference for anyone working on typed tracks/clips, compatibility, track-list visuals, or adding new track properties.

---

## DNA Structure (`DNA_space_types.h`)

```c
struct BetterTimelineTrack {
  BetterTimelineTrack *next, *prev;
  char name[64];        // display name shown in the track list
  char track_type[64];  // idname key into the runtime type registry (max 64 chars)
  char selected;        // non-zero = selected
  char flag;            // eBetterTimelineTrackFlag bitfield
  char _pad0[6];
  ListBaseT<BetterTimelineClip> clips;
  ListBaseT<BetterTimelineTrack> group_tracks; // child tracks when this track is a group
  IDProperty *properties; // type-specific extra data — extend here, not in the struct
  Object *object;          // weak object-slot binding for track types that support it
};
```

**Rule**: never add hardcoded feature fields to this struct. Type-specific state goes in `properties` (IDProperty blob). Global behavioural flags (mute, lock, solo, …) go in `flag`.

Track ownership rules:

- `SpaceBetterTimeline::tracks` is the authoritative **top-level** track list.
- `BetterTimelineTrack::clips` is the authoritative per-track clip list.
- `BetterTimelineTrack::group_tracks` holds child tracks when `track_type == BETTER_TIMELINE_TT_GROUP`. The hierarchy is recursive; nested groups are valid as long as reorder/drop logic preserves the cycle guards.
- `BetterTimelineTrack::object` is the common weak object-slot binding used by track types that set `has_object_slot`. It is not a type-specific payload and must participate in ID walking/remap.
- Do not redesign clip storage as a detached global clip container.

### `flag` bits

| Constant                           | Bit    | Effect                                                        |
|------------------------------------|--------|---------------------------------------------------------------|
| `BETTER_TIMELINE_TRACK_MUTED`      | `1<<0` | Row + canvas darkened; clips rendered grey; "Muted" label     |
| `BETTER_TIMELINE_TRACK_LOCKED`     | `1<<1` | Row + canvas darkened; diagonal hatching; "Locked" label      |
| `BETTER_TIMELINE_TRACK_COLLAPSED`  | `1<<2` | Group only: children hidden; row height stays 1 track row     |

New flags follow the same pattern — add the bit here, add read helpers in `better_timeline_data.cc`, handle visual fallout in `better_timeline_draw.cc`, add click-toggle in `better_timeline_ops_tracks.cc`.

Locked tracks also block clip selection interactions. Their clips are not valid targets for:
- direct click selection
- oskey toggle-selection
- shift range selection
- box selection
- resize handle picking

When a track becomes locked, any existing clip selection on that track is cleared immediately.

---

## Track-Type System

Every track has a **track type** — a runtime-registered descriptor that controls what clip types it accepts, what colour/icon it shows, and (optionally) custom compat logic.

### Struct (`ED_better_timeline.hh`)

```cpp
struct BetterTimelineTrackType {
  char  idname[64];      // e.g. "BETTER_TIMELINE_TT_ANIMATION"
  char  label[64];       // human-readable
  float color[3];        // RGB — used for left accent bar and icon tint
  int   icon;            // BIFIconID — shown next to the track name
  Vector<std::string> compatible_clip_type_ids;
  bool (*clip_type_poll)(...); // override compat check; nullptr = use the id list
};
```

### Built-in types (registered in `better_timeline_types.cc`)

| idname                         | Accent colour (R,G,B)   | Icon                 | Accepts                         | Notes            |
|--------------------------------|-------------------------|----------------------|---------------------------------|------------------|
| `BETTER_TIMELINE_TT_GROUP`     | 0.55 · 0.55 · 0.55      | `ICON_FILE_FOLDER`   | none (container only)           | `is_group = true` |
| `BETTER_TIMELINE_TT_TEST`      | 0.38 · 0.51 · 0.68      | `ICON_SEQ_SEQUENCER` | `BETTER_TIMELINE_CT_TEST`       |                  |
| `BETTER_TIMELINE_TT_ANIMATION` | 0.35 · 0.62 · 0.43      | `ICON_ACTION`        | `BETTER_TIMELINE_CT_ANIMATION`  |                  |
| `BETTER_TIMELINE_TT_SPLINE`    | 0.72 · 0.52 · 0.22      | `ICON_CURVE_DATA`    | `BETTER_TIMELINE_CT_SPLINE`     |                  |

The group type is registered **first** so it appears at the top of the Shift+A menu.

`BetterTimelineTrackType::is_group = true` means: the track is a container, stores children in
`group_tracks`, draws a collapse arrow instead of clip lanes, and accepts no clips itself.

To look up a type at runtime: `ed::better_timeline::track_type_find_from_idname(track->track_type)`.

**Rule**: all compatibility checks must go through the centralised API in `ED_better_timeline.hh`. Never scatter `if (track_type == "...")` conditions across operator code.

## Clip-Type System

Clips are also typed and persist their type idname in `BetterTimelineClip::clip_type`.

Runtime descriptor source:

- `source/blender/editors/include/ED_better_timeline.hh`
- `source/blender/editors/space_better_timeline/better_timeline_types.cc`

Important API entry points:

- `ed::better_timeline::clip_type_find_from_idname()`
- `ed::better_timeline::track_accepts_clip_type()`
- `ed::better_timeline::track_accepts_clip()`
- `ed::better_timeline::track_can_place_clip()`
- `ed::better_timeline::clip_types_allow_overlap()`

Practical rule:

- create, move, paste, duplicate, drag/drop, and inline property edits must all converge on the same compatibility and placement checks

---

---

## Visible-Row System

The track hierarchy is **not** flat. Groups hold child tracks in `group_tracks`. Collapsed groups
hide children. Every piece of code that maps between a Y coordinate or a row index and a track
**must** use the visible-row helpers, not raw `BLI_findlink` or flat index arithmetic.

### Key types and functions (`better_timeline_data.cc`, `better_timeline_intern.hh`)

```cpp
struct BetterTimelineVisibleRow {
  BetterTimelineTrack *track;
  BetterTimelineTrack *parent_group; // nullptr for top-level
  int indent;                        // 0 = top-level, 1 = inside group
};

Vector<BetterTimelineVisibleRow> better_timeline_visible_rows_build(sbetter_timeline);
int          better_timeline_visible_row_count(sbetter_timeline);
BetterTimelineTrack *better_timeline_visible_row_track_get(sbetter_timeline, row_index);
int          better_timeline_visible_row_index_from_track_ptr(sbetter_timeline, track);
```

`better_timeline_visible_rows_build()` rebuilds the list from scratch on every call; do not cache
it across frames (track changes are frequent and the list is cheap to rebuild for typical track
counts).

### `selected_track_index` semantics

`SpaceBetterTimeline::selected_track_index` is a **visible-row index**, not a flat top-level
index. It changes meaning when groups are present. Never use `BLI_findlink(&space->tracks, index)`
to turn it back into a track pointer — use `better_timeline_visible_row_track_get()` instead.

### Flat-list vs visible-row split

Some helpers still work on the **flat top-level list** (`sbetter_timeline->tracks`):

- `better_timeline_track_index_from_ptr()` and `better_timeline_track_count()` — flat-list helpers only
- fallback insertion when there is no active parent group

Group-aware reorder/paste/delete/copy paths must resolve the owning list explicitly: top-level
tracks live in `SpaceBetterTimeline::tracks`, child tracks live in their parent's `group_tracks`.

Code paths that drive UI (Y-to-row mapping, icon/name drawing, mute/lock hit-test, selection,
clip drawing) must use the visible-row helpers. Code paths that affect clips or hidden children
must use recursive all-track traversal where appropriate. Check carefully when adding new operators.

---

## Track-List Row Layout

```
row height: 34 px (BETTER_TIMELINE_ROW_HEIGHT, unscaled)

[1px pad] [4px accent bar] [4px gap] [20px type icon] [4px gap] [track name …] … [20px mute btn] [20px lock btn] [4px margin]
```

All pixel values are unscaled; multiply by `UI_SCALE_FAC` at draw time.

- **Accent bar**: coloured stripe derived from `BetterTimelineTrackType::color`. Drawn with 1 px inset top/bottom/left so bars don't visually merge across rows.
- **Type icon**: `BetterTimelineTrackType::icon`. Alpha 0.80 normally, 0.35 when muted.
- **Mute button**: `ICON_HIDE_OFF` / `ICON_HIDE_ON`. Rect from `better_timeline_track_mute_button_rect()`.
- **Lock button**: `ICON_UNLOCKED` / `ICON_LOCKED`. Rect from `better_timeline_track_lock_button_rect()`.
- Hit-test helpers (`better_timeline_layout.cc`): `better_timeline_track_from_mute_button_region_pos()` / `…_lock_…()`.
- Keyboard toggles: `M` mutes/unmutes all selected tracks as a group, `L` locks/unlocks all selected tracks as a group.

---

## Visual State Summary

| State    | Track-list row                                   | Timeline canvas (right of splitter)             |
|----------|--------------------------------------------------|-------------------------------------------------|
| Normal   | dark grey (`0.19`), full-brightness text/icons   | subtle white overlay on row (`0.055` alpha)     |
| Selected | blue (`0.25, 0.40, 0.72`)                        | lighter blue overlay (`0.20` alpha)             |
| Muted    | + black overlay `0.28` α · dimmed text `×0.55`  | + black overlay `0.22` α · clips desaturated    |
| Locked   | + black overlay `0.28` α                        | + black overlay `0.22` α · diagonal stripes     |

Muted and locked tracks show a **status label** (rounded box) centered horizontally in the canvas at the track's row height. Boxes are always the same size (max of the current label variants + padding). A track with both flags shows `Locked / Muted`.

---

## Adding a New Track State (checklist)

1. Add bit to `eBetterTimelineTrackFlag` in `DNA_space_types.h`
2. Add `better_timeline_track_is_X()` helper in `better_timeline_data.cc` + declare in `better_timeline_intern.hh`
3. Dark overlay in the background geometry pass inside `better_timeline_draw_layout_overlay()` (group with muted/locked pattern)
4. Any extra canvas effect (stripes, clip tint, …) as a separate draw pass
5. Status label if needed — follow the "Locked"/"Muted" label pattern (uniform box size, `ui::draw_roundbox_4fv`, scissored to `body_rect`)
6. Button rect functions in `better_timeline_layout.cc`
7. Click-toggle in `better_timeline_track_select_click_invoke()` with `better_timeline_undo_push_init()`
8. Group hotkeys in `better_timeline_ops_tracks.cc` if the state should apply to the current selected track set (`M`, `L`, future solo/hide toggles, etc.)
