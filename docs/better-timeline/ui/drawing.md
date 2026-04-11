# Drawing System — Passes, APIs & Constraints

> Reference for anyone adding new visual elements to the track list or timeline canvas.
> Unity's Timeline is the visual reference; match its look and feel where possible.

Related docs:

- `docs/better-timeline/ui/layout-and-regions.md` for rect ownership and View2D layout rules
- `docs/better-timeline/architecture/data-model-and-types.md` for track visual states and typed model rules

---

## Two Regions, Two Coordinate Spaces

The main window region has two logical areas drawn in the same region:

| Area              | X range                             | Coordinate space used               |
|-------------------|-------------------------------------|-------------------------------------|
| **Track List Pane** | `0` → `left_panel_width`          | pixel-space (`wmOrtho2_region_pixelspace`) |
| **Timeline Canvas** | `left_panel_width` → `region->winx` | view-space for clips (`better_timeline_view_ortho`) **or** pixel-space for overlays |

`left_panel_width` = `better_timeline_left_panel_width(region, sbetter_timeline)` — respects the user-dragged splitter.

**Rule**: clip drawing uses view-space (frames → pixels via `ui::view2d_view_to_region_x`). All overlays (row backgrounds, icons, labels, buttons) use pixel-space. Never mix them without switching projection.

---

## Scissor Helper

Always use this pair to constrain drawing to a rect:

```cpp
BetterTimelineClipState state;
better_timeline_clip_begin(region, rect_rcti, &state);
// ... draw calls ...
better_timeline_clip_end(state);
```

Key rects:
- `better_timeline_body_rect()` — canvas area (right of panel, below scrub bar). Use for clipping clip drawing and status labels.
- `content_rect = {0, region->winx, 0, content_top}` — full-width strip covering all track rows.

---

## Pass Order in `better_timeline_draw_layout_overlay()`

Everything runs inside a single `wmOrtho2_region_pixelspace` push/pop. Passes must stay in this order to get correct z-layering:

1. **Row backgrounds** — solid filled rects for selected / unselected / muted / locked states
2. **Diagonal stripe pass** — locked tracks only; `GPU_PRIM_LINES` at 45°
3. **Separator lines + vertical divider**
4. **Scrollbar** (via `ui::draw_widget_scroll`)
5. **Accent bars + button bg** — coloured left stripe, subtle button backgrounds
6. **Track name text** — `BLF_draw_default`
7. **Icon pass** — type icon, mute icon, lock icon via `ui::icon_draw_ex`
8. **Status label boxes** — "Locked", "Muted", or "Locked / Muted" rounded boxes + text, scissored to `body_rect`

Status labels have two Better Timeline-specific constraints:
- Clamp the badge against the row's visible slice inside `body_rect`, then keep a small extra
  inset from the top/bottom canvas edge. This avoids the topmost visible muted/locked badge
  looking vertically off when it sits close to the scrub boundary.
- Draw the badge text with the regular UI text path (`ui::style_get_dpi()->widget` +
  `ui::fontstyle_draw`) instead of hand-placing `BLF_draw_default()`. Measuring the box and
  drawing the text through the same UI font path keeps centering stable for the first visible
  status badge too.
- The icon pass uses `ui::icon_draw_ex()` with a theme-derived mono text color instead of relying
  on the default icon tint. This keeps the type/mute/lock icons readable on selected rows and
  dark muted/locked backgrounds.

If you add a new visual layer, decide where in this order it belongs and insert it there. Don't append to the end blindly — icons must always sit above backgrounds.

### Icon Pass Scissor Rule

The icon pass (step 7) **must** be wrapped in `better_timeline_clip_begin / better_timeline_clip_end`
using `content_rect = {0, region->winx, 0, content_top}`. Without this scissor, icons bleed outside
the content area when tracks are scrolled — the icons for the first/last visible row appear above or
below the track panel boundary.

```cpp
better_timeline_clip_begin(region, content_rect, &content_clip_state);
GPU_blend(GPU_BLEND_ALPHA);
// ... icon draw loop ...
GPU_blend(GPU_BLEND_NONE);
better_timeline_clip_end(content_clip_state);
```

---

## GPU Immediate Mode Pattern

```cpp
GPU_blend(GPU_BLEND_ALPHA);
GPUVertFormat *format = immVertexFormat();
uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

immUniformColor4f(r, g, b, a);
immRectf(pos, x0, y0, x1, y1);         // filled rect
// or
immBegin(GPU_PRIM_LINES, count * 2);
  immVertex2f(pos, x0, y0);
  immVertex2f(pos, x1, y1);
immEnd();

immUnbindProgram();
GPU_blend(GPU_BLEND_NONE);
```

**Gotcha**: never share a `pos` attribute between two separate `immVertexFormat()` calls — each `immVertexFormat()` creates a new format; get a fresh `pos` from it.

---

## Rounded Box API

Namespace: `blender::ui` (declared in `UI_interface_c.hh`).

```cpp
ui::draw_roundbox_corner_set(ui::CNR_ALL);  // must call before drawing
const rctf rect = {xmin, xmax, ymin, ymax};

float fill[4]    = {0.12f, 0.12f, 0.12f, 0.88f};
float outline[4] = {0.50f, 0.50f, 0.50f, 0.80f};

ui::draw_roundbox_4fv(&rect, true,  radius, fill);    // filled pass
ui::draw_roundbox_4fv(&rect, false, radius, outline); // outline pass
```

Corner flags: `ui::CNR_TOP_LEFT`, `ui::CNR_TOP_RIGHT`, `ui::CNR_BOTTOM_RIGHT`, `ui::CNR_BOTTOM_LEFT`, `ui::CNR_ALL` (all four), `ui::CNR_NONE`.

**Gotcha**: the identifiers are `ui::CNR_ALL` (not `UI_CNR_ALL`) and `ui::draw_roundbox_4fv` (not `UI_draw_roundbox_4fv`). The old C-style names don't exist in this codebase.

For anti-aliased rounded rects (e.g. dark overlay bars on top of other geometry), prefer the AA variant:

```cpp
ui::draw_roundbox_corner_set(ui::CNR_ALL); // still required
const rctf rect = {xmin, xmax, ymin, ymax};
const float col[4] = {0.0f, 0.0f, 0.0f, 0.82f};
ui::draw_roundbox_aa(&rect, true, radius, col);
```

`draw_roundbox_aa` must be called **outside** any active `immBindBuiltinProgram` block — it manages its own GPU state. Calling it inside an imm block corrupts the GPU pipeline.

---

## Icon Drawing API

```cpp
// blender::ui, declared in UI_interface_c.hh
ui::icon_draw_ex(
    x, y,                // bottom-left corner in pixel-space
    icon_id,             // ICON_* constant
    1.0f / UI_SCALE_FAC, // aspect — keeps icons pixel-perfect at any DPI
    alpha,               // 0.0–1.0
    0.0f,                // desaturate amount (0 = full colour)
    nullptr,             // mono_color (nullptr = use theme colour)
    false,               // mono_border
    nullptr              // IconTextOverlay
);
```

Icon IDs used in the track list (`UI_icons.hh`):

| Usage              | Visible  | Active/On        |
|--------------------|----------|------------------|
| Mute button        | `ICON_HIDE_OFF` | `ICON_HIDE_ON` |
| Lock button        | `ICON_UNLOCKED` | `ICON_LOCKED`  |
| Test track type    | `ICON_SEQ_SEQUENCER` | —         |
| Animation track    | `ICON_ACTION`   | —              |
| Spline track       | `ICON_CURVE_DATA` | —            |
| Object slot (bar)  | `ICON_OBJECT_DATA` | —           |
| Object slot picker | `ICON_TRIA_DOWN` | —             |
| Object type — Mesh | `ICON_OUTLINER_OB_MESH` | —      |
| Object type — Camera | `ICON_OUTLINER_OB_CAMERA` | —  |
| Object type — Light | `ICON_OUTLINER_OB_LIGHT` | —   |
| Object type — Armature | `ICON_OUTLINER_OB_ARMATURE` | — |
| Object type — Empty | `ICON_OUTLINER_OB_EMPTY` | —   |

Note: in Blender 5.1 the old `OB_CURVE` constant is `OB_CURVES`, and `ICON_OUTLINER_OB_CURVES_LEGACY` is `ICON_OUTLINER_OB_CURVES`.

---

## BLF Text API

```cpp
// Measure before drawing to centre or right-align:
float w = BLF_width(BLF_default(), text, BLF_DRAW_STR_DUMMY_MAX);
float h = BLF_height(BLF_default(), text, BLF_DRAW_STR_DUMMY_MAX);

// h is height above baseline only — add ~20–25% for full line height if needed.

// Set colour then draw:
BLF_color4f(BLF_default(), r, g, b, a);
BLF_draw_default(x, y, 0.0f, text, BLF_DRAW_STR_DUMMY_MAX);

// Vertical centre formula:
float text_y = box_center_y - h * 0.5f;
```

---

## Diagonal Stripe Helper

```cpp
// static in better_timeline_draw.cc
better_timeline_draw_diagonal_stripes(x0, y0, x1, y1, pos);
// Draws 45° lines (bottom-left → top-right), spacing 11 px scaled.
// Call with immBind active and colour already set.
// Active scissor handles clipping — lines intentionally extend beyond the rect.
```

---

## Clip Colour & Muted Override

`better_timeline_clip_color_get(clip, color[4])` returns the base colour. Currently hardcoded per clip type. When the parent track is muted, override after the call:

```cpp
if (track_muted) {
  float lum = color[0]*0.2126f + color[1]*0.7152f + color[2]*0.0722f;
  float grey = lum * 0.5f + 0.22f;
  color[0] = color[1] = color[2] = grey;
  color[3] *= 0.55f;
}
```

This desaturates to perceptual grey. Use the same formula for any future "inactive" clip state.

Current caveat:

- clip styling is not fully registry-driven yet; some color decisions still key off `clip_type` idname in draw code. If styling becomes type-extensible, move that responsibility toward the registry/model layer instead of adding more hardcoded string checks here.

---

## Collapsed Group Ghost Clips

When a group track is collapsed, all clips from all descendant tracks are drawn as non-interactive
gray rectangles ("ghost clips") in the group's single row. This gives a visual summary of what is
inside without expanding.

Implementation in `better_timeline_draw_clips()`:

1. `better_timeline_collect_group_clips()` — recursive helper that walks `group->group_tracks`,
   skipping nested group headers, and collects pointers to all leaf clips.
2. The collected `(start_frame, end_frame)` intervals are sorted by start and **merged** so that
   overlapping or adjacent clips (from different tracks or blend regions on the same track) appear
   as a single rectangle rather than stacking.
3. Each merged interval is drawn as a filled gray rect (`0.55, 0.55, 0.55, 0.50`) with a subtle
   outline (`0.70, 0.70, 0.70, 0.35`). Ghost clips are entirely visual — no hit-testing.

**Why merge?** Without merging, two tracks whose clips overlap in time produce visually stacked
rectangles of double opacity, which looks like one wide clip with uneven brightness. Merging gives
one clean solid rect per time region.

**Expanded group**: skip entirely — children will draw their own clips in their own rows.

---

## Constraints & Known Gotchas

- **Do NOT reset `region->v2d.cur` to `tot` every draw** — breaks wheel/MMB zoom navigation.
- **Do NOT enable `V2D_KEEPZOOM`** on the Better Timeline View2D.
- **Do NOT enable `ED_KEYMAP_ANIMATION`** on the window region — crashes in `ED_markers_region_visible()`.
- Ruler scrubbing uses `ANIM_OT_change_frame`; this operator must explicitly allow `SPACE_BETTER_TIMELINE` (already done in `anim_ops.cc`).
- During splitter drag, call `better_timeline_view2d_update_old_window()` to refresh `v2d.oldwinx/oldwiny`, otherwise Blender auto-zooms on every redraw.
- `ED_time_scrub_draw()` must be scissored to the canvas rect, not the full region — otherwise the ruler bleeds under the left panel.
