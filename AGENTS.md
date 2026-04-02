# Better Timeline Notes

This fork adds a custom editor type named `Better Timeline` on top of `blender-v5.1-release`.

Terminology for this editor:
- `Track List Pane`: the left-side pane that shows track names and, later, track type/status UI.
- `Timeline Canvas`: the right-side pane that shows the time ruler, playhead, grid, and later clips.
- `Splitter`: the resizable divider between `Track List Pane` and `Timeline Canvas`.

Current behavior:
- `Better Timeline` appears in the editor type selector under `Animation`.
- The editor uses a clip-oriented custom header with playback and marker controls, but avoids keyframe-specific timeline UI.
- The main region now has a Unity-like shell: left track-side panel, top ruler strip, playhead, frame grid, and shaded areas outside the scene frame range.
- `Track List Pane` now starts empty and exposes a `+` button in its top strip; clicking it appends a new track, assigns its name once at creation time (`Track1`, `Track2`, ...), and selects it.
- There are still no real clip editing interactions yet, but the underlying data model is now typed and clip-capable; track rows are no longer meant to be treated as untyped placeholders in architecture decisions.

Primary files for this editor:
- `source/blender/editors/space_better_timeline/space_better_timeline.cc`
- `source/blender/editors/space_better_timeline/better_timeline_types.cc`
- `source/blender/editors/include/ED_better_timeline.hh`
- `source/blender/editors/animation/anim_ops.cc`
- `source/blender/editors/space_better_timeline/CMakeLists.txt`
- `source/blender/editors/CMakeLists.txt`
- `source/blender/editors/include/ED_space_api.hh`
- `source/blender/editors/space_api/spacetypes.cc`
- `source/blender/makesdna/DNA_space_enums.h`
- `source/blender/makesdna/DNA_space_types.h`
- `source/blender/makesrna/intern/rna_space.cc`
- `source/blender/makesrna/intern/rna_ui.cc`
- `scripts/startup/bl_ui/space_better_timeline.py`
- `scripts/startup/bl_ui/__init__.py`

What each file is for:
- `space_better_timeline.cc`: editor creation, region registration, main timeline shell drawing, track selection behavior, Better Timeline keymap wiring, typed track creation flow, and Better Timeline track/clip DNA copy/free/read-write handling.
- `better_timeline_types.cc`: centralized Better Timeline runtime registry for `TrackType`, `ClipType`, built-in types, and compatibility checks.
- `ED_better_timeline.hh`: public editor-side API for Better Timeline type lookup, registration, and compatibility queries.
- `anim_ops.cc`: shared frame-change/scrubbing operator logic; Better Timeline has to be explicitly allowed there for the ruler to be interactive.
- `ED_space_api.hh` and `spacetypes.cc`: global editor-type registration on startup.
- `DNA_space_enums.h` and `DNA_space_types.h`: persistent space id, DNA structs for `SpaceBetterTimeline`, typed `BetterTimelineTrack`, and typed `BetterTimelineClip`.
- `rna_space.cc`: editor selector menu entry and UI-facing label/icon.
- `rna_ui.cc`: runtime RNA registration hooks for addon-defined Better Timeline `TrackType` and `ClipType`.
- `space_better_timeline.py`: Python header UI, including the standard editor-type switch button.
- `space_better_timeline.py`: Python header UI, menus, playback controls, and clip-oriented top bar behavior.
- `bl_ui/__init__.py`: loads the Better Timeline UI module on startup.

Typed data-model rules that must now be treated as foundational:
- `BetterTimelineTrack` is typed. The persistent identifier lives in `BetterTimelineTrack::track_type` as a string idname.
- `BetterTimelineClip` is typed. The persistent identifier lives in `BetterTimelineClip::clip_type` as a string idname.
- `BetterTimelineTrack::clips` is the authoritative per-track clip ownership list. Do not design future clip storage as a detached global list that later maps back to tracks.
- `BetterTimelineTrack::properties` and `BetterTimelineClip::properties` are the extensible payload roots for type-specific data. Do not replace the typed model with a single shapeless universal blob; common clip fields belong in the base DNA struct and type-specific payload belongs in these properties.
- Compatibility between track types and clip types is a model/runtime rule, not a UI-only validation rule.
- All clip-affecting operations must use the same centralized compatibility logic from the Better Timeline registry/API. This includes create clip, paste, duplicate, drag/drop between tracks, and future import/addon clip creation.
- Do not scatter `if (track_type == ...)` decision logic across editor code. Add new behavior through the registry/model layer and query compatibility/capabilities from there.
- A clip must not be inserted into an incompatible track type, and clip moves across track types must remain denied unless the destination track type explicitly accepts that clip type.
- Addons are expected to be able to register new `TrackType` and `ClipType` descriptors at runtime, and custom track types may define compatibility through a centralized poll/callback rather than hardcoded lists.
- UI should read restrictions from the model. Creation menus and future drag/drop feedback must be driven by registered types and compatibility results, not duplicated hand-written rules.
- The built-in examples currently registered are `Test Track` -> `Test Clip`, `Animation Track` -> `Animation Clip`, and `Spline Track` -> `Spline Clip`.
- The track add menu is now registry-driven. `Shift+A` should evolve by reading registered track types, not by hardcoding literal menu entries.
- Track rows may still look visually simple, but architecturally they are typed tracks already. Do not introduce temporary untyped APIs that would later need to be broken to support real clips.

Current runtime architecture:
- The Better Timeline runtime registry lives in `better_timeline_types.cc` and is exposed through `ED_better_timeline.hh`.
- Built-in Better Timeline track/clip types are registered on demand during Better Timeline setup/read paths; future built-ins should be added there, not ad hoc inside the editor UI code.
- Addon-facing RNA registration for Better Timeline types lives in `rna_ui.cc` as `BetterTimelineTrackType` and `BetterTimelineClipType`.
- `BetterTimelineTrackType::clip_type_poll` is the central extensibility point for custom compatibility logic when a simple static compatibility list is not enough.
- Better Timeline undo/duplicate logic must deep-copy track/clip properties and clip lists. Do not revert to shallow `BLI_duplicatelist()` behavior for tracks now that nested typed data exists.
- Better Timeline blend read/write must handle nested `clips` and `IDProperty` payloads explicitly; plain struct-list serialization is no longer sufficient by itself.

Main drawing pieces already in use:
- `ED_time_scrub_draw()` and `ED_time_scrub_draw_current_frame()` provide the top ruler strip and active-frame marker.
- `ui::view2d_draw_lines_x_frames()` provides the timeline grid.
- `ANIM_draw_framerange()` provides the darkened out-of-range zones before start and after end.
- `ANIM_draw_cfra()` provides the main playhead line in the timeline body.
- `better_timeline_draw_layout_overlay()` in `space_better_timeline.cc` adds the Unity-like shell overlay: left sidebar, row scaffolding, separators, and top accent line.
- The ruler is currently frame-based, not seconds-based.
- Playhead scrubbing/click-to-jump in the top ruler uses manually added standard Blender `Time Scrub` and `Animation` keymaps on the window region, not a Better Timeline-specific operator.
- Do not enable `ED_KEYMAP_ANIMATION` on this editor region directly unless marker support is implemented; that flag also installs marker polling and currently crashes in `ED_markers_region_visible()`.
- `ANIM_OT_change_frame` must explicitly allow `SPACE_BETTER_TIMELINE` in `anim_ops.cc`; without that, the ruler renders but click/drag scrubbing does nothing.
- Playhead snapping is currently disabled for Better Timeline in `anim_ops.cc` until this editor has its own snap target/keylist logic.
- Better Timeline tracks now persist in `SpaceBetterTimeline::tracks` as a DNA `ListBase`; do not regenerate track names from visual row order.
- Better Timeline clips now persist per track in `BetterTimelineTrack::clips`; future clip work must treat those lists as authoritative.
- Track selection is now multi-select on `BetterTimelineTrack::selected`; `SpaceBetterTimeline::selected_track_index` is the active/anchor row for range selection and future context actions.
- The next autogenerated track label is stored in `SpaceBetterTimeline::next_track_name_index`, so deleting/reordering rows must not rename existing tracks.
- The left track column width is also stored in `SpaceBetterTimeline` as `track_panel_width`, so divider resize survives redraws and can later survive more custom layout work.
- Vertical `Track List Pane` scroll offset currently stores in `SpaceBetterTimeline` as `track_scroll_offset`.
- The `+` button in the `Track List Pane` top strip is handled by `BETTER_TIMELINE_OT_add_track`; keep its hit-test separate from scrub, splitter, and row-selection hit-tests.
- `BETTER_TIMELINE_OT_add_track` now accepts a `track_type` RNA string property. When adding new entry points for track creation, pass a registered track type idname rather than creating untyped rows.
- Track deletion is handled by `BETTER_TIMELINE_OT_delete_track` on `Delete` and `X`.
- `Shift`-click extends selection from the active/anchor track and adds the whole range between both rows without clearing already selected tracks.
- `Cmd`-click toggles the clicked row in the current selection without clearing the rest.
- `Esc` clears the current Better Timeline track selection.
- `Space` in Better Timeline is keyed to the standard `SCREEN_OT_animation_play`, so playback toggles without a custom transport operator.
- `Shift+A` opens a small Better Timeline add-track popup only when no tracks are selected; for now it exposes just `Test Track`.
- Placeholder tracks currently still delete immediately if they have no clips; non-empty typed tracks should use the existing confirm path instead of changing keymap behavior.
- Track-structure edits such as add, delete, and reorder are expected to be undoable from Better Timeline with `Cmd+Z` / `Shift+Cmd+Z`; future track/clip editing operators should keep using `OPTYPE_UNDO` and ensure the Better Timeline keymap exposes undo/redo for them.
- Better Timeline editor state lives in `SpaceBetterTimeline` on `bScreen`; in Blender 5.1 `bScreen` uses `IDTYPE_FLAGS_NO_MEMFILE_UNDO`, so track/clip state changes will not participate in regular global memfile undo unless Better Timeline provides its own custom `UndoType`.
- Track selection uses `BETTER_TIMELINE_OT_track_select` on a dedicated `Better Timeline` keymap and passes through clicks in the top scrub bar.
- Mouse wheel over `Track List Pane` is handled by `BETTER_TIMELINE_OT_scroll_tracks` and scrolls rows vertically; wheel over `Timeline Canvas` must continue to pass through to horizontal timeline zoom.
- If the track list does not need vertical scrolling, wheel events over `Track List Pane` should still be consumed there and must not fall through to `Timeline Canvas` zoom.
- The right edge of the window now exposes a vertical track scrollbar; its thumb is driven by the same `track_scroll_offset` and is draggable through `BETTER_TIMELINE_OT_scrollbar_drag`.
- The left/right divider resize uses `BETTER_TIMELINE_OT_resize_panel` on the same keymap and a window-region cursor callback; clicks on the divider must pass through track selection.
- During divider drag, refresh `region->v2d.oldwinx/oldwiny` against the timeline-side mask width/height or Blender will treat the changing mask as a resize-zoom signal and keep zooming out every redraw.
- Do not enable `V2D_KEEPZOOM` on the Better Timeline `View2D`. This editor should keep manual wheel zoom, but region/panel resize must not auto-zoom the timeline.
- `ED_time_scrub_draw()` and `ED_time_scrub_draw_current_frame()` are region-wide by default. In Better Timeline they must be clipped to the right-side timeline rect, or the ruler/current-frame UI will bleed under the left track panel and the split will look like an overlay instead of two panes.
- Horizontal navigation/zoom depends on keeping `region->v2d.cur` persistent; do not reset `cur` to `tot` every draw or wheel/MMB navigation will break.
- `Better Timeline` currently overrides the default `View2D` wheel behavior with its own keymap order: mouse wheel zooms horizontally, and middle mouse pans.
- `Home` uses `BETTER_TIMELINE_OT_view_all`, not generic `VIEW2D_OT_reset`, so the visible range fits the scene `Start/End` frame range inside the timeline mask while keeping the left track column out of the framing logic. Frame the horizontal range from `sfra` to `efra + 1` so the start line sits flush against the timeline-side edge and the last frame stays visible at the right edge.

Likely future touchpoints for Better Timeline work:
- Add operators and keymaps in `space_better_timeline.cc` or sibling files in the same folder.
- Extend the type registry and typed clip/track runtime API in `better_timeline_types.cc` / `ED_better_timeline.hh`.
- Add custom headers, panels, tools, or sub-modes by extending this editor's region types.
- Evolve `space_better_timeline.py` toward a more Unity-style clip timeline header and transport bar.
- Add real clip drawing, selection, editing, and drag/drop in `space_better_timeline.cc` or split that logic into dedicated sibling draw/operator files when it starts growing.
- Add addon-visible typed behavior in `rna_ui.cc` if Better Timeline types need richer registration callbacks later.
- If the editor needs theme-specific colors later, review `source/blender/editors/interface/resources.cc`.

Code organization rules for future Better Timeline work:
- Do not keep growing `source/blender/editors/space_better_timeline/space_better_timeline.cc` as a catch-all file.
- New Better Timeline features should default to a modular file layout in the same folder, following the patterns used by other Blender editor modules such as separate `*_ops.cc`, `*_draw.cc`, `*_utils.cc`, or other focused sibling files when responsibilities are distinct.
- Keep `space_better_timeline.cc` focused on editor/space registration, region setup, top-level lifecycle hooks, and only the glue needed to wire Better Timeline submodules together.
- Put substantial operator implementations, drawing code, data/serialization helpers, and interaction helpers into dedicated sibling translation units instead of appending more large sections to `space_better_timeline.cc`.
- When adding a new Better Timeline subsystem, prefer introducing a narrowly scoped sibling file immediately rather than waiting for another monolithic refactor later.

Working assumption for future changes:
- New timeline-specific UI and behavior should prefer extending `Better Timeline` instead of modifying the stock Dope Sheet/Timeline mode unless there is a clear compatibility reason.
- Do not keep accumulating major Better Timeline logic in `source/blender/editors/space_better_timeline/space_better_timeline.cc`.
- Prefer a modular Blender-style split with sibling `.cc` files grouped by responsibility, for example space/bootstrap, draw, operators/interactions, and shared internal helpers/types.
- When adding substantial new Better Timeline behavior, first place it in the appropriate module or create a new focused sibling file instead of extending the monolith further.
