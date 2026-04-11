# Interactions & Keymap

> Reference for Better Timeline input behavior: mouse hit-testing, gesture priority, and keyboard
> shortcuts. Use this when adding or changing operators in `better_timeline_ops_view.cc`,
> `better_timeline_ops_tracks.cc`, or `better_timeline_ops_clips.cc`.

Related docs:

- `docs/better-timeline/ui/layout-and-regions.md` for hit-test rect ownership
- `docs/better-timeline/workflows/common-change-recipes.md` for shortcut and interaction edit checklists

---

## Where Keybindings Live

- The Better Timeline window-region keymap is ensured in
  `source/blender/editors/space_better_timeline/better_timeline_ops_view.cc`.
- `better_timeline_keymap_ensure()` is the canonical place for Better Timeline-specific hotkeys.
- Add new operators in the focused `*_ops_*.cc` file first, then bind them from the keymap ensure
  function.

Rule: do not hide substantial input behavior inside ad hoc event checks spread across draw or
space-registration code. Bind an explicit operator unless the behavior truly belongs to existing
standard Blender keymaps like `View2D` or `Time Scrub`.

---

## Gesture Priority

Mouse input in the main region is intentionally ordered:

1. scrub bar interactions
2. splitter / scrollbar / add button hit-tests
3. clip edge resize
4. clip drag / clip box-select
5. track-row selection / reorder

This ordering matters. Example: clip resize must be registered before clip drag so edge clicks are
claimed by resize first and only fall through to drag when no resize handle is under the mouse.

### Custom Scrollbar Priority Gotcha

`better_timeline_is_in_timeline_canvas` returns **true** for the scrollbar column because
`body_rect.xmax = region->winx`, which includes the custom scrollbar strip. This means operators
that guard on `!is_in_timeline_canvas` will **not** early-exit when the user clicks the scrollbar.

**Fix**: both `better_timeline_clip_drag_invoke` and `better_timeline_clip_select_invoke` explicitly
check `better_timeline_is_in_track_scrollbar()` and return `OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH`
before any clip or box-select logic.

Do **not** use `ui::view2d_mouse_in_scrollers()` here — it checks View2D's built-in scrollbar, which
is not in use. Better Timeline uses a fully custom scrollbar drawn and hit-tested by
`better_timeline_track_scrollbar_rect()` / `better_timeline_is_in_track_scrollbar()`.

Any new LMB operator that can fire inside the timeline canvas must include this check.

---

## Current Track Shortcuts

- `Left Mouse`: select track row
- `Shift + Left Mouse`: range-select tracks
- `Cmd + Left Mouse`: toggle track in selection
- `Left Mouse Drag` on track list: reorder selected tracks (or drop into a group — see below)
- `Left Mouse` on collapse arrow of a group: expand/collapse the group
- `Shift + A` with no active selection: open the add-track menu
- `Delete`, `X`: delete selected tracks
- `M`: mute/unmute selected tracks
- `L`: lock/unlock selected tracks

### Group Collapse Toggle

The collapse arrow is drawn to the left of the folder icon on every group row. It is detected in
`better_timeline_track_select_click_invoke` using `better_timeline_is_on_group_collapse_toggle()`
**before** the mute/lock and selection logic. A click toggles `BETTER_TIMELINE_TRACK_COLLAPSED`.

When collapsed, child tracks are not emitted by `better_timeline_visible_rows_build()`, so all
row-count, hit-test, and draw operations correctly treat the group as a single row.

### Drag-to-Group

During a track reorder drag (`better_timeline_track_reorder_modal`), the drop behavior depends on
where the cursor is within a group row:

| Cursor position within a group row | Result               |
|------------------------------------|----------------------|
| Top 25% or bottom 25% of row       | Normal insertion line |
| Center 50% of row                  | Drop-into-group highlight; drop moves selected top-level tracks into `group->group_tracks` |

Detection: `better_timeline_detect_group_drop_target()` in `better_timeline_ops_tracks.cc`.

On release with a group target set, `better_timeline_move_selected_tracks_into_group()` removes
selected top-level tracks from `sbetter_timeline->tracks` and appends them to `group->group_tracks`.
The group cannot be dropped into itself (guard: `better_timeline_track_is_selected(hovered)`
returns true for the group itself → no drop target).

Visual feedback: blue tinted fill + border on the group row, insertion line suppressed
(`drop_group_target` field on `BetterTimelineTrackDragVisualState`).

`M` and `L` use group-toggle behavior:
- if any selected track does not already have the state, the key enables it on all selected tracks
- if all selected tracks already have the state, the key disables it on all selected tracks

For `L`, locking a track also clears any selected clips that belong to that track.

---

## Current Clip Shortcuts

- `Left Mouse`: drag clip
- `Shift + Left Mouse`: clip range-select behavior
- `Cmd + Left Mouse`: clip toggle-select
- `Left Mouse Drag` on empty canvas: box-select clips
- `Shift + A` with an active track/clip selection: open the add-clip menu filtered by the selected track type
- `G`: move selected clips horizontally
- `Delete`, `X`: delete selected clips
- `Shift + D`: duplicate selected clips

Locked tracks are intentionally excluded from clip selection and clip resize hit-testing.

---

## Object Slot Click Behavior

The object slot bar on each track (animation tracks, etc.) has three separate click zones handled
inside `better_timeline_track_select_click_invoke`:

1. **Picker sub-rect** (right edge of bar, square = bar height): opens a searchable enum popup
   listing all scene objects. Click is detected first; if it hits the picker rect the operator
   `BETTER_TIMELINE_OT_track_pick_object` is invoked via `WM_operator_name_call_ptr`.

2. **Object icon or visible object-name text**: selects the bound object in the viewport
   (deselect-all + select + activate + `DEG_id_tag_update`) and syncs the Outliner.
   Only fires when `track->object != nullptr`.

3. **Remaining slot-bar background**: does not select the bound object. It falls through to normal
   Better Timeline row-selection behavior.

Priority: picker check runs before the object-select check so a click on the picker corner never
triggers a viewport selection. Object selection is intentionally narrower than the full bar; do not
re-expand it to the whole slot rect unless that UX is explicitly desired.

## Searchable Enum Popup Pattern

`WM_enum_search_invoke` opens a floating search popup for an operator's enum property. To use it:

```cpp
// In the operator type registration:
PropertyRNA *prop = RNA_def_enum(
    ot->srna, "my_prop", rna_enum_dummy_NULL_items, 0, "Label", "Tip");
RNA_def_enum_funcs(prop, my_dynamic_enum_items_fn);
RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
ot->prop = prop;          // required — WM_enum_search_invoke reads ot->prop
ot->invoke = WM_enum_search_invoke;
```

The dynamic callback signature:
```cpp
static const EnumPropertyItem *my_dynamic_enum_items_fn(
    bContext *C, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free);
```

When `*r_free = true`, the returned array is freed by RNA after use. String fields
(`identifier`, `name`) may safely point into live data (e.g. `ob->id.name`) because the array
is consumed immediately before any data mutation.

To invoke with pre-set properties from a click handler:
```cpp
wmOperatorType *ot = WM_operatortype_find("MY_OT_operator", true);
PointerRNA op_props = WM_operator_properties_create_ptr(ot); // returns PointerRNA, not ptr arg
RNA_int_set(&op_props, "track_index", row);
WM_operator_name_call_ptr(C, ot, wm::OpCallContext::InvokeDefault, &op_props, event);
WM_operator_properties_free(&op_props);
```

The `rna_enum_dummy_NULL_items` sentinel lives in `RNA_enum_types.hh` (not `RNA_enum_items.hh`).

## C++ Namespace Gotchas

**`LISTBASE_FOREACH` with `Object *` inside `namespace blender`**: the macro expands to a C-style
cast that the compiler rejects in this context. Use a manual loop instead:

```cpp
for (Object *ob = static_cast<Object *>(bmain->objects.first); ob != nullptr;
     ob = static_cast<Object *>(ob->id.next))
{
  // ...
}
```

**`eObjectSelect_Mode` enum**: values are scoped as `ed::object::BA_SELECT` /
`ed::object::BA_DESELECT`, not bare `BA_SELECT` / `BA_DESELECT`.

## Adding New Keybindings

When adding a Better Timeline keybinding:

1. put the behavior in an operator with `OPTYPE_UNDO` when it changes track/clip state
2. register the operator in the appropriate `*_ops_*.cc`
3. add an idempotent binding in `better_timeline_keymap_ensure()`
4. detect existing bindings by operator idname + concrete key/modifier shape so re-running the
   ensure code does not duplicate entries
5. check for conflicts against existing Better Timeline gestures before choosing the key

Prefer documenting new user-visible shortcuts here once the developer confirms the behavior works.

## No-Op Modal Rule

Clip modal operators that start from generic `Left Mouse` bindings must return `OPERATOR_CANCELLED`
when nothing actually changed.

Current examples:

- no-op `clip_drag` release cancels instead of finishing
- no-op `clip_resize` release cancels instead of finishing
- clip box-select release propagates the result of `better_timeline_track_select_click_invoke()`
  instead of always reporting success

This prevents stray clicks from finalizing an unrelated Better Timeline undo snapshot.
