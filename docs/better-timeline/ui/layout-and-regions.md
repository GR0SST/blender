# Layout And Regions

Use this doc before changing pane geometry, scrollbars, splitter behavior, View2D setup, or hit-testing that depends on layout rects.

## Region Setup

Region creation and registration live in `source/blender/editors/space_better_timeline/space_better_timeline.cc`.

- header region: `RGN_TYPE_HEADER`
- properties region: `RGN_TYPE_UI`
- main region: `RGN_TYPE_WINDOW`

The `Properties Pane` is a normal Blender UI sidebar region. Do not reintroduce a fake right-side panel inside the main window region.

## Main Region Composition

The window region contains two logical panes:

- `Track List Pane` on the left
- `Timeline Canvas` on the right

They share one region and one `View2D`, but layout helpers partition the region into task-specific rects.

The most-used helpers live in `better_timeline_layout.cc` and are declared in `better_timeline_intern.hh`.

Important helpers:

- `better_timeline_left_panel_width()`
- `better_timeline_body_rect()`
- `better_timeline_scrub_rect()`
- `better_timeline_add_button_rect()`
- scrollbar rect helpers
- per-track row / button / clip hit-test helpers

## Stored Layout State

Layout is not purely ephemeral. These fields persist on `SpaceBetterTimeline`:

- `track_panel_width` = width of the `Track List Pane`
- `track_scroll_offset` = vertical scroll offset for track rows

Implications:

- divider resize survives redraws
- track list scroll survives redraws and can survive broader state snapshots
- deleting or reordering tracks must not mutate names or layout state casually

## View2D Rules

The main region uses a custom `View2D` setup from `better_timeline_main_region_init()`.

Critical rules:

- do not enable `V2D_KEEPZOOM`
- do not reset `region->v2d.cur` to `tot` every draw
- horizontal framing should use `BETTER_TIMELINE_OT_view_all`, not generic `VIEW2D_OT_reset`

During splitter drag, `better_timeline_view2d_update_old_window()` must refresh `v2d.oldwinx/oldwiny` against the timeline-side mask size. Without that, Blender interprets the mask change like a resize-zoom and the canvas keeps zooming out.

## Scrollbar And Splitter

The left/right divider and the vertical track scrollbar are custom Better Timeline interactions:

- divider drag operator: `BETTER_TIMELINE_OT_resize_panel`
- track wheel scroll operator: `BETTER_TIMELINE_OT_scroll_tracks`
- scrollbar drag operator: `BETTER_TIMELINE_OT_scrollbar_drag`

Behavior rules:

- clicks on the divider must pass through track selection
- wheel over Track List Pane should scroll rows, not zoom the timeline
- if there is no vertical scrollable content, wheel over Track List Pane should still be consumed there and not fall through to timeline zoom

## Scrub Bar Partition

The scrub ruler is visually part of the main region but logically restricted to the canvas side.

Important consequences:

- scrub hit-tests must respect the timeline-side rect
- `ED_time_scrub_draw()` and `ED_time_scrub_draw_current_frame()` must be clipped to the canvas rect
- add-button, splitter, scrollbar, clip hits, and row selection must remain separate from scrub hits

## Where Layout Changes Usually Land

- rect math and pixel geometry: `better_timeline_layout.cc`
- draw fallout from geometry change: `better_timeline_draw.cc`
- cursor or drag behavior fallout: `better_timeline_ops_view.cc`, `better_timeline_ops_tracks.cc`, `better_timeline_ops_clips.cc`
- persistence of a new layout field: `DNA_space_types.h` + `better_timeline_data.cc` + undo snapshot
