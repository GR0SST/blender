/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "DNA_listBase.h"

#include "BLI_rect.h"

#include "BKE_undo_system.hh"

namespace blender {

struct ARegion;
struct BetterTimelineClip;
struct BetterTimelineTrack;
struct BlendDataReader;
struct BlendWriter;
struct Scene;
struct ScrArea;
struct SpaceLink;
struct SpaceBetterTimeline;
struct View2D;
struct bContext;
struct Main;
struct wmEvent;
struct wmWindowManager;
struct wmRegionListenerParams;
struct wmTimer;

/* internal exports only */

constexpr int BETTER_TIMELINE_MIN_ROWS = 6;
constexpr int BETTER_TIMELINE_ROW_HEIGHT = 34;
constexpr int BETTER_TIMELINE_PANEL_DEFAULT_WIDTH = 270;
constexpr int BETTER_TIMELINE_PANEL_MIN_WIDTH = 180;
constexpr int BETTER_TIMELINE_TIMELINE_MIN_WIDTH = 120;
constexpr int BETTER_TIMELINE_DIVIDER_HIT_WIDTH = 5;
constexpr int BETTER_TIMELINE_ADD_BUTTON_SIZE = 20;
constexpr int BETTER_TIMELINE_ADD_BUTTON_MARGIN = 8;
constexpr int BETTER_TIMELINE_SCROLLBAR_WIDTH = 12;
constexpr int BETTER_TIMELINE_SCROLLBAR_MIN_THUMB_HEIGHT = 28;
constexpr float BETTER_TIMELINE_REORDER_AUTOSCROLL_TIMER_STEP = 0.02f;
constexpr const char *BETTER_TIMELINE_KEYMAP_NAME = "Better Timeline";

struct BetterTimelinePanelResizeData {
  int initial_mouse_x;
  int initial_panel_width;
};

struct BetterTimelineScrollbarDragData {
  int initial_mouse_y;
  int initial_scroll_offset;
};

struct BetterTimelineTrackReorderData {
  BetterTimelineTrack *dragged_track;
  BetterTimelineTrack *active_track;
  int current_insertion_index;
  int last_mouse_y;
  wmTimer *autoscroll_timer;
};

struct BetterTimelineTrackDragVisualState {
  const ARegion *region;
  const BetterTimelineTrack *dragged_track;
  int insertion_index;
  bool active;
};

struct BetterTimelineUndoStep {
  UndoStep step;
  SpaceBetterTimeline *space;
  ListBase tracks;
  int selected_track_index;
  int next_track_name_index;
  int track_panel_width;
  int track_scroll_offset;
};

enum eBetterTimelineTrackScrollDirection {
  BETTER_TIMELINE_TRACK_SCROLL_UP = -1,
  BETTER_TIMELINE_TRACK_SCROLL_DOWN = 1,
};

int better_timeline_panel_width_clamp(const ARegion *region, int panel_width);
int better_timeline_left_panel_width(const ARegion *region,
                                     const SpaceBetterTimeline *sbetter_timeline);
bool better_timeline_is_on_panel_divider(const ARegion *region,
                                         const SpaceBetterTimeline *sbetter_timeline,
                                         int region_x);
void better_timeline_view2d_update_old_window(ARegion *region,
                                              const SpaceBetterTimeline *sbetter_timeline);
void better_timeline_view_sync(ARegion *region,
                               const Scene *scene,
                               const SpaceBetterTimeline *sbetter_timeline);
rcti better_timeline_body_rect(const ARegion *region,
                               const SpaceBetterTimeline *sbetter_timeline);
rcti better_timeline_scrub_rect(const ARegion *region,
                                const SpaceBetterTimeline *sbetter_timeline);
rcti better_timeline_add_button_rect(const ARegion *region,
                                     const SpaceBetterTimeline *sbetter_timeline);
bool better_timeline_is_in_add_button(const ARegion *region,
                                      const SpaceBetterTimeline *sbetter_timeline,
                                      int region_x,
                                      int region_y);
bool better_timeline_scrub_event_in_region(const ScrArea *area,
                                           const ARegion *region,
                                           const wmEvent *event);
int better_timeline_content_height(const ARegion *region);
int better_timeline_track_scroll_max(const ARegion *region,
                                     const SpaceBetterTimeline *sbetter_timeline);
int better_timeline_track_scroll_offset(const ARegion *region,
                                        const SpaceBetterTimeline *sbetter_timeline);
bool better_timeline_track_scrollbar_visible(const ARegion *region,
                                             const SpaceBetterTimeline *sbetter_timeline);
rcti better_timeline_track_scrollbar_rect(const ARegion *region,
                                          const SpaceBetterTimeline *sbetter_timeline);
rcti better_timeline_track_scrollbar_thumb_rect(const ARegion *region,
                                                const SpaceBetterTimeline *sbetter_timeline);
bool better_timeline_is_in_track_scrollbar(const ARegion *region,
                                           const SpaceBetterTimeline *sbetter_timeline,
                                           int region_x,
                                           int region_y);
bool better_timeline_is_in_track_list_pane(const ARegion *region,
                                           const SpaceBetterTimeline *sbetter_timeline,
                                           int region_x);
float better_timeline_row_ymax(const ARegion *region,
                               const SpaceBetterTimeline *sbetter_timeline,
                               int row_index);
float better_timeline_row_ymin(const ARegion *region,
                               const SpaceBetterTimeline *sbetter_timeline,
                               int row_index);
bool better_timeline_row_is_visible(const ARegion *region,
                                    const SpaceBetterTimeline *sbetter_timeline,
                                    int row_index);
int better_timeline_track_from_region_y(const ARegion *region,
                                        const SpaceBetterTimeline *sbetter_timeline,
                                        int region_y);
int better_timeline_track_insertion_index_from_region_y(const ARegion *region,
                                                        const SpaceBetterTimeline *sbetter_timeline,
                                                        int region_y);
bool better_timeline_reorder_selected_tracks_to_insertion_index(
    SpaceBetterTimeline *sbetter_timeline, int insertion_index);
bool better_timeline_track_reorder_autoscroll_apply(
    const ARegion *region, SpaceBetterTimeline *sbetter_timeline, int region_y);
float better_timeline_track_insertion_y(const ARegion *region,
                                        const SpaceBetterTimeline *sbetter_timeline,
                                        int insertion_index);
void better_timeline_track_drag_visual_state_update(const ARegion *region,
                                                    const BetterTimelineTrack *dragged_track,
                                                    int insertion_index);
void better_timeline_track_drag_visual_state_clear();
bool better_timeline_operator_region_poll(bContext *C);
void better_timeline_view_ops_register();
void better_timeline_main_region_keymap_init(wmWindowManager *wm, ARegion *region);
void better_timeline_track_ops_register();
void better_timeline_main_region_draw(const bContext *C, ARegion *region);
void better_timeline_main_region_draw_overlay(const bContext *C, ARegion *region);
void better_timeline_main_region_listener(const wmRegionListenerParams *params);
void better_timeline_space_state_init(SpaceBetterTimeline *sbetter_timeline);
bool better_timeline_track_is_selected(const BetterTimelineTrack *track);
void better_timeline_track_set_selected(BetterTimelineTrack *track, bool selected);
BetterTimelineTrack *better_timeline_track_at_index(SpaceBetterTimeline *sbetter_timeline,
                                                    int track_index);
const BetterTimelineTrack *better_timeline_track_at_index(
    const SpaceBetterTimeline *sbetter_timeline, int track_index);
int better_timeline_track_count(const SpaceBetterTimeline *sbetter_timeline);
int better_timeline_track_index_from_ptr(const SpaceBetterTimeline *sbetter_timeline,
                                         const BetterTimelineTrack *target_track);
BetterTimelineTrack *better_timeline_track_create(const char *track_type_idname, int track_name_index);
void better_timeline_track_free(BetterTimelineTrack *track);
void better_timeline_tracks_free(ListBase *tracks);
void better_timeline_tracks_duplicate(ListBase *dst, const ListBase *src);
const char *better_timeline_track_type_label_get(const BetterTimelineTrack *track);
void better_timeline_track_ensure_type(BetterTimelineTrack *track);
void better_timeline_clip_ensure_type(BetterTimelineTrack *track, BetterTimelineClip *clip);
bool better_timeline_has_selected_track(const SpaceBetterTimeline *sbetter_timeline);
int better_timeline_selected_track_count(const SpaceBetterTimeline *sbetter_timeline);
void better_timeline_clear_selection(SpaceBetterTimeline *sbetter_timeline);
void better_timeline_select_only_track(SpaceBetterTimeline *sbetter_timeline, int track_index);
int better_timeline_first_selected_track_index(const SpaceBetterTimeline *sbetter_timeline);
void better_timeline_space_blend_read_data(BlendDataReader *reader, SpaceLink *sl);
void better_timeline_space_blend_write(BlendWriter *writer, SpaceLink *sl);

}  // namespace blender
