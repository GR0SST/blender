/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "DNA_listBase.h"

#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "BKE_undo_system.hh"

namespace blender {

struct LibraryForeachIDData;
namespace bke::id {
class IDRemapper;
}

struct ARegion;
struct BetterTimelineClip;
struct BetterTimelineTrack;
struct BlendDataReader;
struct BlendLibReader;
struct BlendWriter;
struct ID;
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
constexpr float BETTER_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH = 8.0f;
/** Width/height of track list mute and lock buttons (unscaled pixels). */
constexpr int BETTER_TIMELINE_TRACK_BUTTON_SIZE = 20;
/** Width of the coloured type accent bar on the left edge of each track row (unscaled pixels). */
constexpr int BETTER_TIMELINE_TRACK_ACCENT_WIDTH = 4;

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

struct BetterTimelineClipDragVisualState {
  const ARegion *region;
  const BetterTimelineTrack *source_track;
  const BetterTimelineTrack *target_track;
  const BetterTimelineClip *dragged_clip;
  Vector<const BetterTimelineClip *> moved_clips;
  Vector<const BetterTimelineTrack *> moved_clip_tracks;
  float preview_start_frame;
  float preview_end_frame;
  bool drop_valid;
  bool active;
};

struct BetterTimelineClipBoxSelectVisualState {
  const ARegion *region;
  rcti rect;
  bool active;
};

struct BetterTimelineClipResizeVisualState {
  const ARegion *region;
  const BetterTimelineTrack *track;
  const BetterTimelineClip *clip;
  float preview_start_frame;
  float preview_end_frame;
  bool active;
};

struct SpaceBetterTimeline_Runtime {
  BetterTimelineTrackDragVisualState track_drag_visual_state;
  BetterTimelineClipDragVisualState clip_drag_visual_state;
  BetterTimelineClipBoxSelectVisualState clip_box_select_visual_state;
  BetterTimelineClipResizeVisualState clip_resize_visual_state;
  bool undo_push_pending = false;
};

enum eBetterTimelineClipInteractionMode {
  BETTER_TIMELINE_CLIP_INTERACTION_DRAG = 0,
  BETTER_TIMELINE_CLIP_INTERACTION_BOX_SELECT = 1,
  BETTER_TIMELINE_CLIP_INTERACTION_RESIZE = 2,
};

enum eBetterTimelineClipResizeEdge {
  BETTER_TIMELINE_CLIP_RESIZE_EDGE_NONE = 0,
  BETTER_TIMELINE_CLIP_RESIZE_EDGE_START = 1,
  BETTER_TIMELINE_CLIP_RESIZE_EDGE_END = 2,
};

struct BetterTimelineClipInteractionData {
  eBetterTimelineClipInteractionMode interaction_mode;
};

struct BetterTimelineMovedClipState {
  BetterTimelineTrack *source_track;
  BetterTimelineClip *clip;
  float initial_start_frame;
  float initial_end_frame;
};

struct BetterTimelineUndoState {
  ListBase tracks;
  int selected_track_index;
  int selected_clip_index;
  int next_track_name_index;
  int track_panel_width;
  int track_scroll_offset;
};

struct BetterTimelineClipDragData {
  eBetterTimelineClipInteractionMode interaction_mode;
  BetterTimelineTrack *source_track;
  BetterTimelineTrack *target_track;
  BetterTimelineClip *clip;
  Vector<BetterTimelineMovedClipState> moved_clips;
  float initial_start_frame;
  float initial_end_frame;
  float mouse_start_frame;
  float preview_start_frame;
  float preview_end_frame;
  bool drop_valid;
  bool allow_track_change;
  bool remove_on_cancel;
  int last_mouse_y;
};

struct BetterTimelineClipBoxSelectData {
  eBetterTimelineClipInteractionMode interaction_mode;
  int initial_mouse_x;
  int initial_mouse_y;
  int current_mouse_x;
  int current_mouse_y;
  bool active;
  bool extend;
  bool toggle;
};

struct BetterTimelineClipResizeData {
  eBetterTimelineClipInteractionMode interaction_mode;
  BetterTimelineTrack *track;
  BetterTimelineClip *clip;
  eBetterTimelineClipResizeEdge resize_edge;
  float initial_start_frame;
  float initial_end_frame;
  float mouse_start_frame;
  float preview_start_frame;
  float preview_end_frame;
  /** When true and the clip supports speed scaling (e.g. Animation clips with Shift held),
   *  the resize operation should adjust playback speed instead of trimming the clip boundary.
   *  Not yet implemented; reserved for future Unity-style speed-scale drag. */
  bool speed_scale_mode;
};

struct BetterTimelineUndoStep {
  UndoStep step;
  SpaceBetterTimeline *space;
  BetterTimelineUndoState state_before;
  BetterTimelineUndoState state_after;
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
bool better_timeline_reorder_selected_tracks_would_change(
    const SpaceBetterTimeline *sbetter_timeline, int insertion_index);
bool better_timeline_reorder_selected_tracks_to_insertion_index(
    SpaceBetterTimeline *sbetter_timeline, int insertion_index);
bool better_timeline_track_reorder_autoscroll_apply(
    const ARegion *region, SpaceBetterTimeline *sbetter_timeline, int region_y);
float better_timeline_track_insertion_y(const ARegion *region,
                                        const SpaceBetterTimeline *sbetter_timeline,
                                        int insertion_index);
void better_timeline_track_drag_visual_state_update(const SpaceBetterTimeline *sbetter_timeline,
                                                    const ARegion *region,
                                                    const BetterTimelineTrack *dragged_track,
                                                    int insertion_index);
void better_timeline_track_drag_visual_state_clear(SpaceBetterTimeline *sbetter_timeline);
void better_timeline_clip_drag_visual_state_update(const SpaceBetterTimeline *sbetter_timeline,
                                                   const ARegion *region,
                                                   const BetterTimelineTrack *source_track,
                                                   const BetterTimelineTrack *target_track,
                                                   const BetterTimelineClip *dragged_clip,
                                                   Span<const BetterTimelineClip *> moved_clips,
                                                   Span<const BetterTimelineTrack *> moved_clip_tracks,
                                                   float preview_start_frame,
                                                   float preview_end_frame,
                                                   bool drop_valid);
void better_timeline_clip_drag_visual_state_clear(SpaceBetterTimeline *sbetter_timeline);
bool better_timeline_clip_drag_visual_state_is_dragged_clip(
    const SpaceBetterTimeline *sbetter_timeline, const BetterTimelineClip *clip);
void better_timeline_clip_box_select_visual_state_update(const SpaceBetterTimeline *sbetter_timeline,
                                                         const ARegion *region,
                                                         const rcti &rect);
void better_timeline_clip_box_select_visual_state_clear(SpaceBetterTimeline *sbetter_timeline);
eBetterTimelineClipResizeEdge better_timeline_clip_resize_edge_from_region_position(
    const ARegion *region,
    const SpaceBetterTimeline *sbetter_timeline,
    const BetterTimelineTrack *track,
    const BetterTimelineClip *clip,
    int region_x,
    int region_y);
BetterTimelineClip *better_timeline_clip_for_resize_from_region_position(
    const ARegion *region,
    SpaceBetterTimeline *sbetter_timeline,
    int region_x,
    int region_y,
    BetterTimelineTrack **r_track,
    eBetterTimelineClipResizeEdge *r_edge);
void better_timeline_clip_resize_visual_state_update(const SpaceBetterTimeline *sbetter_timeline,
                                                     const ARegion *region,
                                                     const BetterTimelineTrack *track,
                                                     const BetterTimelineClip *clip,
                                                     float preview_start_frame,
                                                     float preview_end_frame);
void better_timeline_clip_resize_visual_state_clear(SpaceBetterTimeline *sbetter_timeline);
bool better_timeline_clip_resize_visual_state_is_resized_clip(
    const SpaceBetterTimeline *sbetter_timeline, const BetterTimelineClip *clip);
bool better_timeline_operator_region_poll(bContext *C);
void better_timeline_view_ops_register();
void better_timeline_main_region_keymap_init(wmWindowManager *wm, ARegion *region);
void better_timeline_track_ops_register();
void better_timeline_clip_ops_register();
void better_timeline_clipboard_track_ops_register();
void better_timeline_clipboard_clip_ops_register();
void better_timeline_drop_register();
wmOperatorStatus better_timeline_track_select_click_invoke(bContext *C, const wmEvent *event);
void better_timeline_main_region_draw(const bContext *C, ARegion *region);
void better_timeline_main_region_draw_overlay(const bContext *C, ARegion *region);
void better_timeline_main_region_listener(const wmRegionListenerParams *params);
void better_timeline_space_state_init(SpaceBetterTimeline *sbetter_timeline);
void better_timeline_space_runtime_free(SpaceBetterTimeline *sbetter_timeline);
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
BetterTimelineTrack *better_timeline_track_duplicate(const BetterTimelineTrack *track_src);
void better_timeline_track_free(BetterTimelineTrack *track);
void better_timeline_tracks_free(ListBase *tracks);
void better_timeline_tracks_duplicate(ListBase *dst, const ListBase *src);
void better_timeline_track_assign_duplicate_name(const SpaceBetterTimeline *sbetter_timeline,
                                                 BetterTimelineTrack *track,
                                                 StringRef source_name);
const char *better_timeline_track_type_label_get(const BetterTimelineTrack *track);
void better_timeline_track_ensure_type(BetterTimelineTrack *track);
BetterTimelineClip *better_timeline_clip_create(const BetterTimelineTrack *track,
                                                const char *clip_type_idname,
                                                float start_frame,
                                                float end_frame);
BetterTimelineClip *better_timeline_clip_duplicate(const BetterTimelineClip *clip_src);
void better_timeline_clip_assign_duplicate_name(const SpaceBetterTimeline *sbetter_timeline,
                                                BetterTimelineClip *clip,
                                                StringRef source_name);
bool better_timeline_clip_range_overlaps(float start_frame_a,
                                         float end_frame_a,
                                         float start_frame_b,
                                         float end_frame_b);
bool better_timeline_track_can_place_clip(
    const BetterTimelineTrack *track,
    StringRef clip_type_idname,
    float start_frame,
    float end_frame,
    const BetterTimelineClip *ignore_clip = nullptr,
    Span<const BetterTimelineClip *> ignored_clips = {});
const BetterTimelineClip *better_timeline_clip_covering_frame(const BetterTimelineTrack *track,
                                                              float frame,
                                                              StringRef clip_type_idname);
float better_timeline_clip_creation_start_frame(const BetterTimelineTrack *track,
                                                StringRef clip_type_idname,
                                                float requested_start_frame,
                                                float duration_frames);
const char *better_timeline_clip_type_label_get(const BetterTimelineClip *clip);
bool better_timeline_clip_is_selected(const BetterTimelineClip *clip);
void better_timeline_clip_set_selected(BetterTimelineClip *clip, bool selected);
void better_timeline_clip_ensure_type(BetterTimelineTrack *track, BetterTimelineClip *clip);
bool better_timeline_has_selected_track(const SpaceBetterTimeline *sbetter_timeline);
int better_timeline_selected_track_count(const SpaceBetterTimeline *sbetter_timeline);
void better_timeline_clear_selection(SpaceBetterTimeline *sbetter_timeline);
void better_timeline_select_only_track(SpaceBetterTimeline *sbetter_timeline, int track_index);
int better_timeline_first_selected_track_index(const SpaceBetterTimeline *sbetter_timeline);
bool better_timeline_has_selected_clip(const SpaceBetterTimeline *sbetter_timeline);
void better_timeline_clear_clip_selection(SpaceBetterTimeline *sbetter_timeline);
void better_timeline_clear_clip_selection_for_track(SpaceBetterTimeline *sbetter_timeline,
                                                    BetterTimelineTrack *track);
int better_timeline_first_selected_clip_index(const SpaceBetterTimeline *sbetter_timeline);
int better_timeline_clip_global_index_from_ptr(const SpaceBetterTimeline *sbetter_timeline,
                                               const BetterTimelineTrack *track,
                                               const BetterTimelineClip *clip);
BetterTimelineClip *better_timeline_clip_at_global_index(SpaceBetterTimeline *sbetter_timeline,
                                                         int clip_index,
                                                         BetterTimelineTrack **r_track);
BetterTimelineClip *better_timeline_clip_from_region_position(
    const ARegion *region,
    SpaceBetterTimeline *sbetter_timeline,
    int region_x,
    int region_y,
    BetterTimelineTrack **r_track);
void better_timeline_space_blend_read_data(BlendDataReader *reader, SpaceLink *sl);
void better_timeline_space_blend_read_after_liblink(BlendLibReader *reader,
                                                    ID *parent_id,
                                                    SpaceLink *sl);
void better_timeline_space_blend_write(BlendWriter *writer, SpaceLink *sl);
void better_timeline_space_id_remap(ScrArea *area,
                                    SpaceLink *sl,
                                    const bke::id::IDRemapper &mappings);
void better_timeline_space_foreach_id(SpaceLink *space_link, LibraryForeachIDData *data);

bool better_timeline_track_is_muted(const BetterTimelineTrack *track);
bool better_timeline_track_is_locked(const BetterTimelineTrack *track);
rcti better_timeline_track_object_slot_rect(const ARegion *region,
                                             const SpaceBetterTimeline *sbetter_timeline,
                                             int row_index);
rcti better_timeline_track_object_slot_picker_rect(const ARegion *region,
                                                   const SpaceBetterTimeline *sbetter_timeline,
                                                   int row_index);
rcti better_timeline_track_mute_button_rect(const ARegion *region,
                                            const SpaceBetterTimeline *sbetter_timeline,
                                            int row_index);
rcti better_timeline_track_lock_button_rect(const ARegion *region,
                                            const SpaceBetterTimeline *sbetter_timeline,
                                            int row_index);
int better_timeline_track_from_mute_button_region_pos(const ARegion *region,
                                                      const SpaceBetterTimeline *sbetter_timeline,
                                                      int region_x,
                                                      int region_y);
int better_timeline_track_from_lock_button_region_pos(const ARegion *region,
                                                      const SpaceBetterTimeline *sbetter_timeline,
                                                      int region_x,
                                                      int region_y);

}  // namespace blender
