# Tracks — Data Model, Types & Visual States

> Reference for anyone working on track behaviour, track-list visuals, or adding new track properties.

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
  IDProperty *properties; // type-specific extra data — extend here, not in the struct
};
```

**Rule**: never add hardcoded feature fields to this struct. Type-specific state goes in `properties` (IDProperty blob). Global behavioural flags (mute, lock, solo, …) go in `flag`.

### `flag` bits

| Constant                        | Bit    | Effect                                                      |
|---------------------------------|--------|-------------------------------------------------------------|
| `BETTER_TIMELINE_TRACK_MUTED`   | `1<<0` | Row + canvas darkened; clips rendered grey; "Muted" label   |
| `BETTER_TIMELINE_TRACK_LOCKED`  | `1<<1` | Row + canvas darkened; diagonal hatching; "Locked" label    |

New flags follow the same pattern — add the bit here, add read helpers in `better_timeline_data.cc`, handle visual fallout in `better_timeline_draw.cc`, add click-toggle in `better_timeline_ops_tracks.cc`.

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

| idname                         | Accent colour (R,G,B)   | Icon                 | Accepts                        |
|--------------------------------|-------------------------|----------------------|--------------------------------|
| `BETTER_TIMELINE_TT_TEST`      | 0.38 · 0.51 · 0.68      | `ICON_SEQ_SEQUENCER` | `BETTER_TIMELINE_CT_TEST`      |
| `BETTER_TIMELINE_TT_ANIMATION` | 0.35 · 0.62 · 0.43      | `ICON_ACTION`        | `BETTER_TIMELINE_CT_ANIMATION` |
| `BETTER_TIMELINE_TT_SPLINE`    | 0.72 · 0.52 · 0.22      | `ICON_CURVE_DATA`    | `BETTER_TIMELINE_CT_SPLINE`    |

To look up a type at runtime: `ed::better_timeline::track_type_find_from_idname(track->track_type)`.

**Rule**: all compatibility checks must go through the centralised API in `ED_better_timeline.hh`. Never scatter `if (track_type == "...")` conditions across operator code.

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

---

## Visual State Summary

| State    | Track-list row                                   | Timeline canvas (right of splitter)             |
|----------|--------------------------------------------------|-------------------------------------------------|
| Normal   | dark grey (`0.19`), full-brightness text/icons   | subtle white overlay on row (`0.055` alpha)     |
| Selected | blue (`0.25, 0.40, 0.72`)                        | lighter blue overlay (`0.20` alpha)             |
| Muted    | + black overlay `0.28` α · dimmed text `×0.55`  | + black overlay `0.22` α · clips desaturated    |
| Locked   | + black overlay `0.28` α                        | + black overlay `0.22` α · diagonal stripes     |

Both muted and locked show a **status label** (rounded box) centered horizontally in the canvas at the track's row height. Boxes are always the same size (max of "Muted"/"Locked" text extents + padding).

---

## Adding a New Track State (checklist)

1. Add bit to `eBetterTimelineTrackFlag` in `DNA_space_types.h`
2. Add `better_timeline_track_is_X()` helper in `better_timeline_data.cc` + declare in `better_timeline_intern.hh`
3. Dark overlay in the background geometry pass inside `better_timeline_draw_layout_overlay()` (group with muted/locked pattern)
4. Any extra canvas effect (stripes, clip tint, …) as a separate draw pass
5. Status label if needed — follow the "Locked"/"Muted" label pattern (uniform box size, `ui::draw_roundbox_4fv`, scissored to `body_rect`)
6. Button rect functions in `better_timeline_layout.cc`
7. Click-toggle in `better_timeline_track_select_click_invoke()` with `better_timeline_undo_push_init()`
