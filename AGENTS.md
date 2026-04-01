# Better Timeline Notes

This fork adds a custom editor type named `Better Timeline` on top of `blender-v5.1-release`.

Current behavior:
- `Better Timeline` appears in the editor type selector under `Animation`.
- The editor uses a clip-oriented custom header with playback and marker controls, but avoids keyframe-specific timeline UI.
- The main region now has a Unity-like shell: left track-side panel, top ruler strip, playhead, frame grid, and shaded areas outside the scene frame range.
- There are no real tracks or clips yet; current row strips are only layout scaffolding for future timeline work.

Primary files for this editor:
- `source/blender/editors/space_better_timeline/space_better_timeline.cc`
- `source/blender/editors/animation/anim_ops.cc`
- `source/blender/editors/space_better_timeline/CMakeLists.txt`
- `source/blender/editors/CMakeLists.txt`
- `source/blender/editors/include/ED_space_api.hh`
- `source/blender/editors/space_api/spacetypes.cc`
- `source/blender/makesdna/DNA_space_enums.h`
- `source/blender/makesdna/DNA_space_types.h`
- `source/blender/makesrna/intern/rna_space.cc`
- `scripts/startup/bl_ui/space_better_timeline.py`
- `scripts/startup/bl_ui/__init__.py`

What each file is for:
- `space_better_timeline.cc`: editor creation, region registration, main timeline shell drawing, track selection behavior, and Better Timeline keymap wiring.
- `anim_ops.cc`: shared frame-change/scrubbing operator logic; Better Timeline has to be explicitly allowed there for the ruler to be interactive.
- `ED_space_api.hh` and `spacetypes.cc`: global editor-type registration on startup.
- `DNA_space_enums.h` and `DNA_space_types.h`: persistent space id, DNA struct, and Better Timeline selection state storage.
- `rna_space.cc`: editor selector menu entry and UI-facing label/icon.
- `space_better_timeline.py`: Python header UI, including the standard editor-type switch button.
- `space_better_timeline.py`: Python header UI, menus, playback controls, and clip-oriented top bar behavior.
- `bl_ui/__init__.py`: loads the Better Timeline UI module on startup.

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
- Track selection currently stores `selected_track_index` directly in `SpaceBetterTimeline`.
- The left track column width is also stored in `SpaceBetterTimeline` as `track_panel_width`, so divider resize survives redraws and can later survive more custom layout work.
- Track selection uses `BETTER_TIMELINE_OT_track_select` on a dedicated `Better Timeline` keymap and passes through clicks in the top scrub bar.
- The left/right divider resize uses `BETTER_TIMELINE_OT_resize_panel` on the same keymap and a window-region cursor callback; clicks on the divider must pass through track selection.
- Horizontal navigation/zoom depends on keeping `region->v2d.cur` persistent; do not reset `cur` to `tot` every draw or wheel/MMB navigation will break.
- `Better Timeline` currently overrides the default `View2D` wheel behavior with its own keymap order: mouse wheel zooms horizontally, and middle mouse pans.
- `Home` uses `BETTER_TIMELINE_OT_view_all`, not generic `VIEW2D_OT_reset`, so the visible range fits the scene `Start/End` frame range inside the timeline mask while keeping the left track column out of the framing logic. Frame the horizontal range from `sfra` to `efra + 1` so the start line sits flush against the timeline-side edge and the last frame stays visible at the right edge.

Likely future touchpoints for Better Timeline work:
- Add operators and keymaps in `space_better_timeline.cc` or sibling files in the same folder.
- Add custom headers, panels, tools, or sub-modes by extending this editor's region types.
- Evolve `space_better_timeline.py` toward a more Unity-style clip timeline header and transport bar.
- Add real track data, clip drawing, selection, and drag/drop in `space_better_timeline.cc` or split that logic into dedicated sibling draw/operator files when it starts growing.
- If the editor needs theme-specific colors later, review `source/blender/editors/interface/resources.cc`.

Working assumption for future changes:
- New timeline-specific UI and behavior should prefer extending `Better Timeline` instead of modifying the stock Dope Sheet/Timeline mode unless there is a clear compatibility reason.
