# Interactions & Keymap

> Reference for Better Timeline input behavior: mouse hit-testing, gesture priority, and keyboard
> shortcuts. Use this when adding or changing operators in `better_timeline_ops_view.cc`,
> `better_timeline_ops_tracks.cc`, or `better_timeline_ops_clips.cc`.

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

---

## Current Track Shortcuts

- `Left Mouse`: select track row
- `Shift + Left Mouse`: range-select tracks
- `Cmd + Left Mouse`: toggle track in selection
- `Left Mouse Drag` on track list: reorder selected tracks
- `Delete`, `X`: delete selected tracks
- `M`: mute/unmute selected tracks
- `L`: lock/unlock selected tracks

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
- `G`: move selected clips horizontally
- `Delete`, `X`: delete selected clips
- `Shift + D`: duplicate selected clips

Locked tracks are intentionally excluded from clip selection and clip resize hit-testing.

---

## Adding New Keybindings

When adding a Better Timeline keybinding:

1. put the behavior in an operator with `OPTYPE_UNDO` when it changes track/clip state
2. register the operator in the appropriate `*_ops_*.cc`
3. add an idempotent binding in `better_timeline_keymap_ensure()`
4. detect existing bindings by operator idname + concrete key/modifier shape so re-running the
   ensure code does not duplicate entries
5. check for conflicts against existing Better Timeline gestures before choosing the key

Prefer documenting new user-visible shortcuts here once the developer confirms the behavior works.
