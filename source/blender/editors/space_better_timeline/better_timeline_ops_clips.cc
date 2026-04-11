/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase.h"

#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_undo_system.hh"

#include "ED_better_timeline.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "better_timeline_intern.hh" /* own include */

namespace blender {

static constexpr float BETTER_TIMELINE_CLIP_VERTICAL_MARGIN = 6.0f;
static constexpr float BETTER_TIMELINE_CLIP_MIN_WIDTH = 10.0f;
static ListBase g_better_timeline_clip_clipboard = {nullptr, nullptr};
static float g_better_timeline_clip_clipboard_anchor_start = 0.0f;

static void better_timeline_undo_push_init(bContext *C, const char *name)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr || wm->runtime->undo_stack == nullptr || wm->op_undo_depth > 1) {
    return;
  }

  if (ScrArea *area = CTX_wm_area(C)) {
    if (auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first)) {
      if (sbetter_timeline->runtime != nullptr) {
        sbetter_timeline->runtime->undo_push_pending = true;
      }
    }
  }

  BKE_undosys_step_push_init(wm->runtime->undo_stack, C, name);
}

static void better_timeline_tag_space_state_changed(bContext *C)
{
  Main *bmain = CTX_data_main(C);
  if (bmain != nullptr) {
    bmain->is_memfile_undo_flush_needed = true;
  }

  WM_event_add_notifier(C, NC_SCREEN | NA_EDITED, nullptr);
}

static BetterTimelineTrack *better_timeline_active_selected_track_get(
    SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return nullptr;
  }

  BetterTimelineTrack *track = better_timeline_visible_row_track_get(
      sbetter_timeline, sbetter_timeline->selected_track_index);
  if (track != nullptr && better_timeline_track_is_selected(track)) {
    return track;
  }

  const int selected_index = better_timeline_first_selected_track_index(sbetter_timeline);
  return better_timeline_visible_row_track_get(sbetter_timeline, selected_index);
}

static BetterTimelineClip *better_timeline_active_selected_clip_get(
    SpaceBetterTimeline *sbetter_timeline, BetterTimelineTrack **r_track)
{
  if (r_track != nullptr) {
    *r_track = nullptr;
  }
  if (sbetter_timeline == nullptr) {
    return nullptr;
  }

  BetterTimelineTrack *track = nullptr;
  BetterTimelineClip *clip = better_timeline_clip_at_global_index(
      sbetter_timeline, sbetter_timeline->selected_clip_index, &track);
  if (clip != nullptr && better_timeline_clip_is_selected(clip)) {
    if (r_track != nullptr) {
      *r_track = track;
    }
    return clip;
  }

  for (const BetterTimelineVisibleRow &row : better_timeline_all_tracks_build(sbetter_timeline)) {
    track = row.track;
    if (better_timeline_track_is_group(track)) {
      continue;
    }
    for (clip = static_cast<BetterTimelineClip *>(track->clips.first); clip != nullptr;
         clip = clip->next)
    {
      if (better_timeline_clip_is_selected(clip)) {
        if (r_track != nullptr) {
          *r_track = track;
        }
        return clip;
      }
    }
  }
  return nullptr;
}

struct BetterTimelineClipVisualOrderItem {
  BetterTimelineTrack *track;
  BetterTimelineClip *clip;
  int global_index;
  int local_index;
};

static Vector<BetterTimelineClipVisualOrderItem> better_timeline_clips_in_visual_order(
    SpaceBetterTimeline *sbetter_timeline)
{
  Vector<BetterTimelineClipVisualOrderItem> ordered_clips;
  if (sbetter_timeline == nullptr) {
    return ordered_clips;
  }

  int global_index = 0;
  const Vector<BetterTimelineVisibleRow> all_rows = better_timeline_all_tracks_build(sbetter_timeline);
  for (const BetterTimelineVisibleRow &row : all_rows) {
    BetterTimelineTrack *track = row.track;
    if (better_timeline_track_is_group(track)) {
      continue;
    }
    if (better_timeline_track_is_locked(track)) {
      global_index += BLI_listbase_count(&track->clips);
      continue;
    }

    Vector<BetterTimelineClipVisualOrderItem> track_clips;
    int local_index = 0;
    for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first); clip != nullptr;
         clip = clip->next, global_index++, local_index++)
    {
      track_clips.append({track, clip, global_index, local_index});
    }

    std::sort(track_clips.begin(),
              track_clips.end(),
              [](const BetterTimelineClipVisualOrderItem &a,
                 const BetterTimelineClipVisualOrderItem &b) {
                if (a.clip->start_frame != b.clip->start_frame) {
                  return a.clip->start_frame < b.clip->start_frame;
                }
                if (a.clip->end_frame != b.clip->end_frame) {
                  return a.clip->end_frame < b.clip->end_frame;
                }
                return a.local_index < b.local_index;
              });

    ordered_clips.extend(track_clips);
  }

  return ordered_clips;
}

static int better_timeline_clip_visual_index_from_global_index(
    SpaceBetterTimeline *sbetter_timeline,
    const Span<BetterTimelineClipVisualOrderItem> ordered_clips,
    const int global_index)
{
  if (sbetter_timeline == nullptr || global_index < 0) {
    return -1;
  }

  BetterTimelineTrack *track = nullptr;
  BetterTimelineClip *clip = better_timeline_clip_at_global_index(
      sbetter_timeline, global_index, &track);
  if (track == nullptr || clip == nullptr) {
    return -1;
  }

  for (const int visual_index : ordered_clips.index_range()) {
    const BetterTimelineClipVisualOrderItem &item = ordered_clips[visual_index];
    if (item.track == track && item.clip == clip) {
      return visual_index;
    }
  }
  return -1;
}

static void better_timeline_clip_clipboard_clear()
{
  BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(g_better_timeline_clip_clipboard.first);
  while (clip != nullptr) {
    BetterTimelineClip *clip_next = clip->next;
    if (clip->properties != nullptr) {
      IDP_FreeProperty(clip->properties);
      clip->properties = nullptr;
    }
    MEM_delete(clip);
    clip = clip_next;
  }
  BLI_listbase_clear(&g_better_timeline_clip_clipboard);
  g_better_timeline_clip_clipboard_anchor_start = 0.0f;
}

static Vector<const BetterTimelineClip *> better_timeline_drag_ignored_clips(
    const BetterTimelineClipDragData &drag_data)
{
  Vector<const BetterTimelineClip *> ignored_clips;
  ignored_clips.reserve(drag_data.moved_clips.size());
  for (const BetterTimelineMovedClipState &clip_state : drag_data.moved_clips) {
    ignored_clips.append(clip_state.clip);
  }
  return ignored_clips;
}

static Vector<const BetterTimelineTrack *> better_timeline_drag_preview_tracks(
    const BetterTimelineClipDragData &drag_data)
{
  Vector<const BetterTimelineTrack *> tracks;
  tracks.reserve(drag_data.moved_clips.size());
  for (const BetterTimelineMovedClipState &clip_state : drag_data.moved_clips) {
    const BetterTimelineTrack *preview_track = clip_state.source_track;
    if (drag_data.allow_track_change && drag_data.target_track != nullptr) {
      preview_track = drag_data.target_track;
    }
    tracks.append(preview_track);
  }
  return tracks;
}

static bool better_timeline_is_in_timeline_canvas(const ARegion *region,
                                                  const SpaceBetterTimeline *sbetter_timeline,
                                                  const int region_x,
                                                  const int region_y)
{
  const rcti body_rect = better_timeline_body_rect(region, sbetter_timeline);
  return BLI_rcti_isect_pt(&body_rect, region_x, region_y);
}

static bool better_timeline_clip_box_select_rect_get(const ARegion *region,
                                                     const SpaceBetterTimeline *sbetter_timeline,
                                                     const int start_x,
                                                     const int start_y,
                                                     const int end_x,
                                                     const int end_y,
                                                     rcti *r_rect)
{
  BLI_assert(r_rect != nullptr);

  rcti unclamped_rect{};
  unclamped_rect.xmin = std::min(start_x, end_x);
  unclamped_rect.xmax = std::max(start_x, end_x);
  unclamped_rect.ymin = std::min(start_y, end_y);
  unclamped_rect.ymax = std::max(start_y, end_y);

  const rcti body_rect = better_timeline_body_rect(region, sbetter_timeline);
  return BLI_rcti_isect(&unclamped_rect, &body_rect, r_rect);
}

static rctf better_timeline_clip_rect(const ARegion *region,
                                      const SpaceBetterTimeline *sbetter_timeline,
                                      const View2D *v2d,
                                      const BetterTimelineTrack *track,
                                      const BetterTimelineClip *clip)
{
  /* Use the visible-row index so tracks inside groups get the correct Y position.
   * better_timeline_track_index_from_ptr only walks the top-level list and returns -1
   * for group children. */
  const int row_index = better_timeline_visible_row_index_from_track_ptr(sbetter_timeline, track);
  const float row_y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
  const float row_y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
  const float clip_y_max = row_y_max - (BETTER_TIMELINE_CLIP_VERTICAL_MARGIN * UI_SCALE_FAC);
  const float clip_y_min = row_y_min + (BETTER_TIMELINE_CLIP_VERTICAL_MARGIN * UI_SCALE_FAC);
  const float start_x = ui::view2d_view_to_region_x(v2d, clip->start_frame);
  const float end_x = std::max(ui::view2d_view_to_region_x(v2d, clip->end_frame),
                               start_x + (BETTER_TIMELINE_CLIP_MIN_WIDTH * UI_SCALE_FAC));
  return {start_x, end_x, clip_y_min, clip_y_max};
}

BetterTimelineClip *better_timeline_clip_from_region_position(const ARegion *region,
                                                              SpaceBetterTimeline *sbetter_timeline,
                                                              const int region_x,
                                                              const int region_y,
                                                              BetterTimelineTrack **r_track)
{
  if (r_track != nullptr) {
    *r_track = nullptr;
  }
  if (region == nullptr || sbetter_timeline == nullptr ||
      !better_timeline_is_in_timeline_canvas(region, sbetter_timeline, region_x, region_y))
  {
    return nullptr;
  }

  const int row_index = better_timeline_track_from_region_y(region, sbetter_timeline, region_y);
  BetterTimelineTrack *track = better_timeline_visible_row_track_get(sbetter_timeline, row_index);
  if (track == nullptr || better_timeline_track_is_locked(track)) {
    return nullptr;
  }

  if (r_track != nullptr) {
    *r_track = track;
  }

  const View2D *v2d = &region->v2d;
  BetterTimelineClip *closest_clip = nullptr;
  float closest_distance = FLT_MAX;
  for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first);
       clip != nullptr;
       clip = clip->next)
  {
    const rctf clip_rect = better_timeline_clip_rect(region, sbetter_timeline, v2d, track, clip);
    if (!BLI_rctf_isect_pt(&clip_rect, float(region_x), float(region_y))) {
      continue;
    }
    const float distance = std::abs((clip_rect.xmin + clip_rect.xmax) * 0.5f - float(region_x));
    if (closest_clip == nullptr || distance < closest_distance) {
      closest_clip = clip;
      closest_distance = distance;
    }
  }
  return closest_clip;
}

static bool better_timeline_clip_intersects_box_selection(
    const ARegion *region,
    const SpaceBetterTimeline *sbetter_timeline,
    const View2D *v2d,
    const BetterTimelineTrack *track,
    const BetterTimelineClip *clip,
    const rcti &selection_rect)
{
  const rctf clip_rect = better_timeline_clip_rect(region, sbetter_timeline, v2d, track, clip);
  return !(clip_rect.xmax < float(selection_rect.xmin) ||
           clip_rect.xmin > float(selection_rect.xmax) ||
           clip_rect.ymax < float(selection_rect.ymin) ||
           clip_rect.ymin > float(selection_rect.ymax));
}

static bool better_timeline_clip_box_select_apply(bContext *C,
                                                  const rcti &selection_rect,
                                                  const bool extend,
                                                  const bool toggle)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);
  const View2D *v2d = &region->v2d;

  better_timeline_clear_selection(sbetter_timeline);
  if (!extend && !toggle) {
    better_timeline_clear_clip_selection(sbetter_timeline);
  }

  bool any_intersection = false;
  for (const BetterTimelineVisibleRow &row : better_timeline_visible_rows_build(sbetter_timeline)) {
    BetterTimelineTrack *track = row.track;
    if (better_timeline_track_is_group(track) || better_timeline_track_is_locked(track)) {
      continue;
    }

    for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first);
         clip != nullptr;
         clip = clip->next)
    {
      if (!better_timeline_clip_intersects_box_selection(
              region, sbetter_timeline, v2d, track, clip, selection_rect))
      {
        continue;
      }

      any_intersection = true;
      if (toggle) {
        better_timeline_clip_set_selected(clip, !better_timeline_clip_is_selected(clip));
      }
      else {
        better_timeline_clip_set_selected(clip, true);
      }
    }
  }

  sbetter_timeline->selected_clip_index = better_timeline_first_selected_clip_index(sbetter_timeline);
  ED_area_tag_redraw(area);
  return any_intersection;
}

static bool better_timeline_add_clip_poll(bContext *C)
{
  if (!better_timeline_operator_region_poll(C)) {
    return false;
  }

  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);
  if (CTX_data_scene(C) == nullptr) {
    return false;
  }
  if (better_timeline_has_selected_track(sbetter_timeline)) {
    return true;
  }
  return better_timeline_has_selected_clip(sbetter_timeline);
}

static wmOperatorStatus better_timeline_add_clip_exec(bContext *C, wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);
  Scene *scene = CTX_data_scene(C);

  BetterTimelineTrack *track = better_timeline_active_selected_track_get(sbetter_timeline);
  if (track == nullptr) {
    BetterTimelineTrack *clip_track = nullptr;
    better_timeline_active_selected_clip_get(sbetter_timeline, &clip_track);
    track = clip_track;
  }
  if (track == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No selected Better Timeline track is available");
    return OPERATOR_CANCELLED;
  }
  if (better_timeline_track_is_locked(track)) {
    BKE_report(op->reports, RPT_ERROR, "Cannot add a clip to a locked Better Timeline track");
    return OPERATOR_CANCELLED;
  }

  char clip_type_idname[ed::better_timeline::BETTER_TIMELINE_TYPE_IDNAME_MAX];
  RNA_string_get(op->ptr, "clip_type", clip_type_idname);
  if (clip_type_idname[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No Better Timeline clip type was specified");
    return OPERATOR_CANCELLED;
  }

  if (!ed::better_timeline::track_accepts_clip_type(*track, clip_type_idname)) {
    BKE_report(op->reports, RPT_ERROR, "Selected track type does not accept that clip type");
    return OPERATOR_CANCELLED;
  }

  const float requested_start_frame = BKE_scene_frame_get(scene);
  const float default_duration_frames = std::max(1.0f, float(scene->frames_per_second()));
  const float start_frame = better_timeline_clip_creation_start_frame(
      track, clip_type_idname, requested_start_frame, default_duration_frames);
  const float end_frame = start_frame + default_duration_frames;

  if (!better_timeline_track_can_place_clip(track, clip_type_idname, start_frame, end_frame)) {
    BKE_report(op->reports, RPT_ERROR, "No legal placement is available for that clip");
    return OPERATOR_CANCELLED;
  }

  better_timeline_undo_push_init(C, op->type->name);
  BetterTimelineClip *clip = better_timeline_clip_create(
      track, clip_type_idname, start_frame, end_frame);
  if (clip == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Failed to create Better Timeline clip");
    return OPERATOR_CANCELLED;
  }

  BLI_addtail(&track->clips, clip);
  better_timeline_tag_space_state_changed(C);
  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static bool better_timeline_clip_select_poll(bContext *C)
{
  return better_timeline_operator_region_poll(C);
}

static wmOperatorStatus better_timeline_clip_select_invoke(bContext *C,
                                                           wmOperator * /*op*/,
                                                           const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);
  const bool shift = (event->modifier & KM_SHIFT) != 0;
  const bool oskey = (event->modifier & KM_OSKEY) != 0;
  if (better_timeline_scrub_event_in_region(area, region, event) ||
      better_timeline_is_in_add_button(region, sbetter_timeline, event->mval[0], event->mval[1]) ||
      better_timeline_is_on_panel_divider(region, sbetter_timeline, event->mval[0]) ||
      better_timeline_is_in_track_scrollbar(
          region, sbetter_timeline, event->mval[0], event->mval[1]))
  {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  BetterTimelineTrack *track = nullptr;
  BetterTimelineClip *clip = better_timeline_clip_from_region_position(
      region, sbetter_timeline, event->mval[0], event->mval[1], &track);
  if (clip == nullptr || track == nullptr) {
    if (!shift && !oskey && better_timeline_has_selected_clip(sbetter_timeline) &&
        better_timeline_is_in_timeline_canvas(region, sbetter_timeline, event->mval[0], event->mval[1]))
    {
      better_timeline_clear_clip_selection(sbetter_timeline);
      ED_area_tag_redraw(area);
      return OPERATOR_FINISHED;
    }
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  const int clicked_clip_index = better_timeline_clip_global_index_from_ptr(
      sbetter_timeline, track, clip);
  better_timeline_clear_selection(sbetter_timeline);

  if (shift) {
    int anchor_index = sbetter_timeline->selected_clip_index;
    if (anchor_index < 0) {
      anchor_index = better_timeline_first_selected_clip_index(sbetter_timeline);
    }
    if (anchor_index < 0) {
      anchor_index = clicked_clip_index;
    }

    const Vector<BetterTimelineClipVisualOrderItem> ordered_clips =
        better_timeline_clips_in_visual_order(sbetter_timeline);
    const int anchor_visual_index = better_timeline_clip_visual_index_from_global_index(
        sbetter_timeline, ordered_clips, anchor_index);
    const int clicked_visual_index = better_timeline_clip_visual_index_from_global_index(
        sbetter_timeline, ordered_clips, clicked_clip_index);

    if (anchor_visual_index < 0 || clicked_visual_index < 0) {
      better_timeline_clip_set_selected(clip, true);
    }
    else {
      const int range_min = std::min(anchor_visual_index, clicked_visual_index);
      const int range_max = std::max(anchor_visual_index, clicked_visual_index);
      for (int visual_index = range_min; visual_index <= range_max; visual_index++) {
        better_timeline_clip_set_selected(ordered_clips[visual_index].clip, true);
      }
    }
    sbetter_timeline->selected_clip_index = clicked_clip_index;
  }
  else if (oskey) {
    const bool was_selected = better_timeline_clip_is_selected(clip);
    better_timeline_clip_set_selected(clip, !was_selected);
    if (!was_selected) {
      sbetter_timeline->selected_clip_index = clicked_clip_index;
    }
    else if (sbetter_timeline->selected_clip_index == clicked_clip_index) {
      sbetter_timeline->selected_clip_index = better_timeline_first_selected_clip_index(
          sbetter_timeline);
    }
  }
  else {
    better_timeline_clear_clip_selection(sbetter_timeline);
    better_timeline_clip_set_selected(clip, true);
    sbetter_timeline->selected_clip_index = clicked_clip_index;
  }

  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_clip_select(wmOperatorType *ot)
{
  ot->name = "Select Better Timeline Clip";
  ot->idname = "BETTER_TIMELINE_OT_clip_select";
  ot->description = "Select a clip in Better Timeline";

  ot->invoke = better_timeline_clip_select_invoke;
  ot->poll = better_timeline_clip_select_poll;

  ot->flag = OPTYPE_INTERNAL;
}

static bool better_timeline_clip_drag_poll(bContext *C)
{
  return better_timeline_operator_region_poll(C);
}

static void better_timeline_clip_box_select_finish(bContext *C, wmOperator *op)
{
  auto *box_select_data = static_cast<BetterTimelineClipBoxSelectData *>(op->customdata);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);
  better_timeline_clip_box_select_visual_state_clear(sbetter_timeline);
  if (box_select_data != nullptr) {
    MEM_delete(box_select_data);
    op->customdata = nullptr;
  }
  ED_area_tag_redraw(CTX_wm_area(C));
}

static bool better_timeline_clip_move_poll(bContext *C)
{
  if (!better_timeline_operator_region_poll(C)) {
    return false;
  }

  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);
  return better_timeline_has_selected_clip(sbetter_timeline);
}

static bool better_timeline_clip_requires_delete_confirm(
    const SpaceBetterTimeline *sbetter_timeline)
{
  return sbetter_timeline != nullptr && better_timeline_has_selected_clip(sbetter_timeline);
}

static void better_timeline_clip_drag_finish(
    bContext *C, wmOperator *op, const bool apply_changes, const bool restore_cursor)
{
  auto *drag_data = static_cast<BetterTimelineClipDragData *>(op->customdata);
  if (drag_data == nullptr) {
    return;
  }

  if (apply_changes) {
    const float frame_delta = drag_data->preview_start_frame - drag_data->initial_start_frame;
    const bool move_track = drag_data->target_track != nullptr &&
                            drag_data->target_track != drag_data->source_track;
    bool changed = move_track || frame_delta != 0.0f;

    if (changed) {
      better_timeline_undo_push_init(C, op->type->name);
      for (const BetterTimelineMovedClipState &clip_state : drag_data->moved_clips) {
        clip_state.clip->start_frame = clip_state.initial_start_frame + frame_delta;
        clip_state.clip->end_frame = clip_state.initial_end_frame + frame_delta;
        if (move_track) {
          BLI_remlink(&clip_state.source_track->clips, clip_state.clip);
          BLI_addtail(&drag_data->target_track->clips, clip_state.clip);
        }
      }
      better_timeline_tag_space_state_changed(C);
    }
  }
  else if (drag_data->remove_on_cancel) {
    for (const BetterTimelineMovedClipState &clip_state : drag_data->moved_clips) {
      BLI_remlink(&clip_state.source_track->clips, clip_state.clip);
      if (clip_state.clip->properties != nullptr) {
        IDP_FreeProperty(clip_state.clip->properties);
        clip_state.clip->properties = nullptr;
      }
      MEM_delete(clip_state.clip);
    }
    better_timeline_tag_space_state_changed(C);
  }

  better_timeline_clip_drag_visual_state_clear(static_cast<SpaceBetterTimeline *>(
      CTX_wm_area(C)->spacedata.first));
  if (restore_cursor) {
    WM_cursor_modal_restore(CTX_wm_window(C));
  }
  MEM_delete(drag_data);
  op->customdata = nullptr;
  ED_area_tag_redraw(CTX_wm_area(C));
}

static bool better_timeline_clip_drag_has_changes(const BetterTimelineClipDragData *drag_data)
{
  if (drag_data == nullptr) {
    return false;
  }

  const float frame_delta = drag_data->preview_start_frame - drag_data->initial_start_frame;
  const bool move_track = drag_data->target_track != nullptr &&
                          drag_data->target_track != drag_data->source_track;
  return move_track || frame_delta != 0.0f;
}

static Vector<const BetterTimelineClip *> better_timeline_dragged_clip_ptrs(
    const BetterTimelineClipDragData &drag_data)
{
  Vector<const BetterTimelineClip *> clips;
  clips.reserve(drag_data.moved_clips.size());
  for (const BetterTimelineMovedClipState &clip_state : drag_data.moved_clips) {
    clips.append(clip_state.clip);
  }
  return clips;
}

static bool better_timeline_clip_drag_start(bContext *C,
                                            wmOperator *op,
                                            const wmEvent *event,
                                            BetterTimelineTrack *track,
                                            BetterTimelineClip *clip,
                                            const bool allow_track_change)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  if (clip == nullptr || track == nullptr) {
    return false;
  }

  if (!better_timeline_clip_is_selected(clip)) {
    better_timeline_clear_selection(sbetter_timeline);
    better_timeline_clear_clip_selection(sbetter_timeline);
    better_timeline_clip_set_selected(clip, true);
    sbetter_timeline->selected_clip_index = better_timeline_clip_global_index_from_ptr(
        sbetter_timeline, track, clip);
  }

  auto *drag_data = MEM_new<BetterTimelineClipDragData>(__func__);
  drag_data->interaction_mode = BETTER_TIMELINE_CLIP_INTERACTION_DRAG;
  drag_data->source_track = track;
  drag_data->target_track = track;
  drag_data->clip = clip;
  drag_data->initial_start_frame = clip->start_frame;
  drag_data->initial_end_frame = clip->end_frame;
  drag_data->mouse_start_frame = ui::view2d_region_to_view_x(&region->v2d, event->mval[0]);
  drag_data->preview_start_frame = clip->start_frame;
  drag_data->preview_end_frame = clip->end_frame;
  drag_data->drop_valid = true;
  drag_data->allow_track_change = allow_track_change;
  drag_data->remove_on_cancel = false;
  drag_data->last_mouse_y = event->mval[1];

  for (const BetterTimelineVisibleRow &sel_row : better_timeline_all_tracks_build(sbetter_timeline)) {
    BetterTimelineTrack *selected_track = sel_row.track;
    if (better_timeline_track_is_group(selected_track)) {
      continue;
    }
    for (BetterTimelineClip *selected_clip = static_cast<BetterTimelineClip *>(selected_track->clips.first);
         selected_clip != nullptr;
         selected_clip = selected_clip->next)
    {
      if (!better_timeline_clip_is_selected(selected_clip)) {
        continue;
      }
      drag_data->moved_clips.append(
          {selected_track, selected_clip, selected_clip->start_frame, selected_clip->end_frame});
    }
  }

  if (drag_data->moved_clips.is_empty()) {
    drag_data->moved_clips.append({track, clip, clip->start_frame, clip->end_frame});
  }

  if (drag_data->moved_clips.size() > 1) {
    drag_data->allow_track_change = false;
  }

  const Vector<const BetterTimelineClip *> ignored_clips = better_timeline_drag_ignored_clips(*drag_data);
  drag_data->drop_valid = true;
  for (const BetterTimelineMovedClipState &clip_state : drag_data->moved_clips) {
    if (!better_timeline_track_can_place_clip(clip_state.source_track,
                                              clip_state.clip->clip_type,
                                              clip_state.initial_start_frame,
                                              clip_state.initial_end_frame,
                                              clip_state.clip,
                                              ignored_clips))
    {
      drag_data->drop_valid = false;
      break;
    }
  }

  op->customdata = drag_data;

  const Vector<const BetterTimelineClip *> moved_clips = better_timeline_dragged_clip_ptrs(*drag_data);
  const Vector<const BetterTimelineTrack *> moved_clip_tracks = better_timeline_drag_preview_tracks(
      *drag_data);
  better_timeline_clip_drag_visual_state_update(
      sbetter_timeline,
      region,
      track,
      track,
      clip,
      moved_clips,
      moved_clip_tracks,
      clip->start_frame,
      clip->end_frame,
      drag_data->drop_valid);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_EW_SCROLL);
  WM_event_add_modal_handler(C, op);
  ED_area_tag_redraw(area);
  return true;
}

static wmOperatorStatus better_timeline_clip_drag_modal(bContext *C,
                                                        wmOperator *op,
                                                        const wmEvent *event)
{
  auto *drag_data = static_cast<BetterTimelineClipDragData *>(op->customdata);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  auto update_preview = [&]() {
    const float mouse_frame = ui::view2d_region_to_view_x(&region->v2d, event->mval[0]);
    const float frame_delta = std::round(mouse_frame - drag_data->mouse_start_frame);
    BetterTimelineTrack *target_track = nullptr;
    if (drag_data->allow_track_change) {
      const int target_index = better_timeline_track_from_region_y(
          region, sbetter_timeline, event->mval[1]);
      if (target_index >= 0) {
        target_track = better_timeline_visible_row_track_get(sbetter_timeline, target_index);
      }
    }
    else {
      target_track = drag_data->source_track;
    }

    const float duration = drag_data->initial_end_frame - drag_data->initial_start_frame;
    const float preview_start = drag_data->initial_start_frame + frame_delta;
    const float preview_end = preview_start + duration;
    bool drop_valid = target_track != nullptr;
    const Vector<const BetterTimelineClip *> ignored_clips = better_timeline_drag_ignored_clips(
        *drag_data);
    if (drop_valid && drag_data->allow_track_change) {
      for (const BetterTimelineMovedClipState &clip_state : drag_data->moved_clips) {
        if (!ed::better_timeline::track_accepts_clip(*target_track, *clip_state.clip)) {
          drop_valid = false;
          break;
        }
      }
    }
    if (drop_valid) {
      for (const BetterTimelineMovedClipState &clip_state : drag_data->moved_clips) {
        BetterTimelineTrack *placement_track = (drag_data->allow_track_change && target_track != nullptr) ?
                                                   target_track :
                                                   clip_state.source_track;
        if (!better_timeline_track_can_place_clip(placement_track,
                                                  clip_state.clip->clip_type,
                                                  clip_state.initial_start_frame + frame_delta,
                                                  clip_state.initial_end_frame + frame_delta,
                                                  clip_state.clip,
                                                  ignored_clips))
        {
          drop_valid = false;
          break;
        }
      }
    }
    drag_data->target_track = target_track;
    drag_data->preview_start_frame = preview_start;
    drag_data->preview_end_frame = preview_end;
    drag_data->drop_valid = drop_valid;
    drag_data->last_mouse_y = event->mval[1];
    const Vector<const BetterTimelineClip *> moved_clips = better_timeline_dragged_clip_ptrs(*drag_data);
    const Vector<const BetterTimelineTrack *> moved_clip_tracks = better_timeline_drag_preview_tracks(
        *drag_data);
    better_timeline_clip_drag_visual_state_update(sbetter_timeline,
                                                  region,
                                                  drag_data->source_track,
                                                  target_track,
                                                  drag_data->clip,
                                                  moved_clips,
                                                  moved_clip_tracks,
                                                  preview_start,
                                                  preview_end,
                                                  drop_valid);
  };

  switch (event->type) {
    case MOUSEMOVE:
      update_preview();
      ED_area_tag_redraw(area);
      break;
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        const bool apply_changes = drag_data->drop_valid && drag_data->target_track != nullptr;
        const bool changed = apply_changes && better_timeline_clip_drag_has_changes(drag_data);
        better_timeline_clip_drag_finish(C, op, apply_changes, true);
        return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      }
      break;
    case EVT_RETKEY:
    case EVT_SPACEKEY:
      if (event->val == KM_PRESS) {
        const bool apply_changes = drag_data->drop_valid && drag_data->target_track != nullptr;
        const bool changed = apply_changes && better_timeline_clip_drag_has_changes(drag_data);
        better_timeline_clip_drag_finish(C, op, apply_changes, true);
        return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      }
      break;
    case RIGHTMOUSE:
    case EVT_ESCKEY:
      if (event->type == EVT_ESCKEY || event->val == KM_PRESS) {
        better_timeline_clip_drag_finish(C, op, false, true);
        return OPERATOR_CANCELLED;
      }
    default:
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus better_timeline_clip_box_select_modal(bContext *C,
                                                              wmOperator *op,
                                                              const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);
  auto *box_select_data = static_cast<BetterTimelineClipBoxSelectData *>(op->customdata);
  if (box_select_data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  auto update_visual_rect = [&]() {
    rcti selection_rect{};
    if (better_timeline_clip_box_select_rect_get(region,
                                                 sbetter_timeline,
                                                 box_select_data->initial_mouse_x,
                                                 box_select_data->initial_mouse_y,
                                                 box_select_data->current_mouse_x,
                                                 box_select_data->current_mouse_y,
                                                 &selection_rect))
    {
      better_timeline_clip_box_select_visual_state_update(
          sbetter_timeline, region, selection_rect);
    }
    else {
      better_timeline_clip_box_select_visual_state_clear(sbetter_timeline);
    }
  };

  switch (event->type) {
    case MOUSEMOVE: {
      box_select_data->current_mouse_x = event->mval[0];
      box_select_data->current_mouse_y = event->mval[1];

      if (!box_select_data->active) {
        const int drag_delta[2] = {box_select_data->current_mouse_x - box_select_data->initial_mouse_x,
                                   box_select_data->current_mouse_y -
                                       box_select_data->initial_mouse_y};
        box_select_data->active = WM_event_drag_test_with_delta(event, drag_delta);
      }

      if (box_select_data->active) {
        update_visual_rect();
        ED_area_tag_redraw(area);
      }
      break;
    }
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        box_select_data->current_mouse_x = event->mval[0];
        box_select_data->current_mouse_y = event->mval[1];

        if (box_select_data->active) {
          rcti selection_rect{};
          if (better_timeline_clip_box_select_rect_get(region,
                                                       sbetter_timeline,
                                                       box_select_data->initial_mouse_x,
                                                       box_select_data->initial_mouse_y,
                                                       box_select_data->current_mouse_x,
                                                       box_select_data->current_mouse_y,
                                                       &selection_rect))
          {
            better_timeline_clip_box_select_apply(
                C, selection_rect, box_select_data->extend, box_select_data->toggle);
          }
        }
        else {
          const wmOperatorStatus click_status = better_timeline_track_select_click_invoke(C, event);
          better_timeline_clip_box_select_finish(C, op);
          return (click_status & OPERATOR_FINISHED) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
        }

        better_timeline_clip_box_select_finish(C, op);
        return OPERATOR_FINISHED;
      }
      break;
    case RIGHTMOUSE:
    case EVT_ESCKEY:
      if (event->type == EVT_ESCKEY || event->val == KM_PRESS) {
        better_timeline_clip_box_select_finish(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    default:
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

/* Forward declaration: defined later in the resize operator section. */
static wmOperatorStatus better_timeline_clip_resize_modal(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent *event);

static wmOperatorStatus better_timeline_clip_interaction_modal(bContext *C,
                                                               wmOperator *op,
                                                               const wmEvent *event)
{
  auto *interaction_data = static_cast<BetterTimelineClipInteractionData *>(op->customdata);
  if (interaction_data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  switch (interaction_data->interaction_mode) {
    case BETTER_TIMELINE_CLIP_INTERACTION_DRAG:
      return better_timeline_clip_drag_modal(C, op, event);
    case BETTER_TIMELINE_CLIP_INTERACTION_BOX_SELECT:
      return better_timeline_clip_box_select_modal(C, op, event);
    case BETTER_TIMELINE_CLIP_INTERACTION_RESIZE:
      return better_timeline_clip_resize_modal(C, op, event);
  }

  return OPERATOR_CANCELLED;
}

static wmOperatorStatus better_timeline_clip_drag_invoke(bContext *C,
                                                         wmOperator *op,
                                                         const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);
  const bool shift = (event->modifier & KM_SHIFT) != 0;
  const bool oskey = (event->modifier & KM_OSKEY) != 0;

  if (better_timeline_scrub_event_in_region(area, region, event) ||
      !better_timeline_is_in_timeline_canvas(
          region, sbetter_timeline, event->mval[0], event->mval[1]))
  {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  /* Custom track scrollbar takes priority: pass through so scrollbar_drag can handle it. */
  if (better_timeline_is_in_track_scrollbar(
          region, sbetter_timeline, event->mval[0], event->mval[1]))
  {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  BetterTimelineTrack *track = nullptr;
  BetterTimelineClip *clip = better_timeline_clip_from_region_position(
      region, sbetter_timeline, event->mval[0], event->mval[1], &track);
  if (clip == nullptr || track == nullptr) {
    auto *box_select_data = MEM_new<BetterTimelineClipBoxSelectData>(__func__);
    box_select_data->interaction_mode = BETTER_TIMELINE_CLIP_INTERACTION_BOX_SELECT;
    box_select_data->initial_mouse_x = event->mval[0];
    box_select_data->initial_mouse_y = event->mval[1];
    box_select_data->current_mouse_x = event->mval[0];
    box_select_data->current_mouse_y = event->mval[1];
    box_select_data->active = false;
    box_select_data->extend = shift;
    box_select_data->toggle = oskey;

    op->customdata = box_select_data;
    WM_event_add_modal_handler(C, op);
    return OPERATOR_RUNNING_MODAL;
  }

  if (shift || oskey) {
    return better_timeline_clip_select_invoke(C, op, event);
  }

  if (!better_timeline_clip_drag_start(C, op, event, track, clip, true)) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  return OPERATOR_RUNNING_MODAL;
}

static void BETTER_TIMELINE_OT_clip_drag(wmOperatorType *ot)
{
  ot->name = "Drag Better Timeline Clip";
  ot->idname = "BETTER_TIMELINE_OT_clip_drag";
  ot->description = "Move a Better Timeline clip between frames and compatible tracks";

  ot->invoke = better_timeline_clip_drag_invoke;
  ot->modal = better_timeline_clip_interaction_modal;
  ot->poll = better_timeline_clip_drag_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

static wmOperatorStatus better_timeline_move_clip_invoke(bContext *C,
                                                         wmOperator *op,
                                                         const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  for (const BetterTimelineVisibleRow &row : better_timeline_all_tracks_build(sbetter_timeline)) {
    BetterTimelineTrack *track = row.track;
    if (better_timeline_track_is_group(track)) {
      continue;
    }
    for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first);
         clip != nullptr;
         clip = clip->next)
    {
      if (better_timeline_clip_is_selected(clip)) {
        if (better_timeline_clip_drag_start(C, op, event, track, clip, false)) {
          return OPERATOR_RUNNING_MODAL;
        }
        return OPERATOR_CANCELLED;
      }
    }
  }

  return OPERATOR_CANCELLED;
}

static wmOperatorStatus better_timeline_delete_clip_exec(bContext *C, wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  if (!better_timeline_has_selected_clip(sbetter_timeline)) {
    return OPERATOR_CANCELLED;
  }

  better_timeline_undo_push_init(C, op->type->name);
  const Vector<BetterTimelineVisibleRow> all_rows = better_timeline_all_tracks_build(sbetter_timeline);
  for (const BetterTimelineVisibleRow &row : all_rows) {
    BetterTimelineTrack *track = row.track;
    if (better_timeline_track_is_group(track)) {
      continue;
    }
    BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first);
    while (clip != nullptr) {
      BetterTimelineClip *clip_next = clip->next;
      if (better_timeline_clip_is_selected(clip)) {
        BLI_remlink(&track->clips, clip);
        if (clip->properties != nullptr) {
          IDP_FreeProperty(clip->properties);
          clip->properties = nullptr;
        }
        MEM_delete(clip);
      }
      clip = clip_next;
    }
  }

  sbetter_timeline->selected_clip_index = -1;
  better_timeline_tag_space_state_changed(C);
  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus better_timeline_delete_clip_invoke(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent * /*event*/)
{
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);

  if (better_timeline_clip_requires_delete_confirm(sbetter_timeline) &&
      RNA_boolean_get(op->ptr, "confirm"))
  {
    return WM_operator_confirm_ex(C,
                                  op,
                                  "Delete selected clip?",
                                  nullptr,
                                  "Delete",
                                  ui::AlertIcon::None,
                                  false);
  }

  return better_timeline_delete_clip_exec(C, op);
}

static bool better_timeline_delete_clip_poll(bContext *C)
{
  if (!better_timeline_operator_region_poll(C)) {
    return false;
  }

  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);
  return better_timeline_has_selected_clip(sbetter_timeline);
}

static void BETTER_TIMELINE_OT_move_clip(wmOperatorType *ot)
{
  ot->name = "Move Better Timeline Clip";
  ot->idname = "BETTER_TIMELINE_OT_move_clip";
  ot->description = "Move selected Better Timeline clips horizontally with the mouse";

  ot->invoke = better_timeline_move_clip_invoke;
  ot->modal = better_timeline_clip_drag_modal;
  ot->poll = better_timeline_clip_move_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

static void BETTER_TIMELINE_OT_delete_clip(wmOperatorType *ot)
{
  ot->name = "Delete Better Timeline Clip";
  ot->idname = "BETTER_TIMELINE_OT_delete_clip";
  ot->description = "Delete selected clips from Better Timeline";

  ot->invoke = better_timeline_delete_clip_invoke;
  ot->exec = better_timeline_delete_clip_exec;
  ot->poll = better_timeline_delete_clip_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  WM_operator_properties_confirm_or_exec(ot);
}

static void BETTER_TIMELINE_OT_add_clip(wmOperatorType *ot)
{
  ot->name = "Add Better Timeline Clip";
  ot->idname = "BETTER_TIMELINE_OT_add_clip";
  ot->description = "Add a compatible clip to the selected Better Timeline track at the playhead";

  ot->exec = better_timeline_add_clip_exec;
  ot->poll = better_timeline_add_clip_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_string(ot->srna,
                 "clip_type",
                 nullptr,
                 ed::better_timeline::BETTER_TIMELINE_TYPE_IDNAME_MAX,
                 "Clip Type",
                 "Registered Better Timeline clip type to create");
}

static bool better_timeline_duplicate_clip_poll(bContext *C)
{
  if (!better_timeline_operator_region_poll(C)) {
    return false;
  }
  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);
  return better_timeline_has_selected_clip(sbetter_timeline);
}

static wmOperatorStatus better_timeline_duplicate_clip_invoke(bContext *C,
                                                              wmOperator *op,
                                                              const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  if (!better_timeline_has_selected_clip(sbetter_timeline)) {
    return OPERATOR_CANCELLED;
  }

  better_timeline_undo_push_init(C, op->type->name);
  better_timeline_clear_selection(sbetter_timeline);

  BetterTimelineClip *active_duplicate = nullptr;
  BetterTimelineTrack *active_track = nullptr;
  for (const BetterTimelineVisibleRow &row : better_timeline_all_tracks_build(sbetter_timeline)) {
    BetterTimelineTrack *track = row.track;
    if (better_timeline_track_is_group(track)) {
      continue;
    }
    Vector<BetterTimelineClip *> selected_source_clips;
    for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first); clip != nullptr;
         clip = clip->next)
    {
      if (better_timeline_clip_is_selected(clip)) {
        selected_source_clips.append(clip);
      }
    }

    for (BetterTimelineClip *source_clip : selected_source_clips) {
      BetterTimelineClip *duplicated_clip = better_timeline_clip_duplicate(source_clip);
      if (duplicated_clip == nullptr) {
        continue;
      }
      better_timeline_clip_assign_duplicate_name(sbetter_timeline, duplicated_clip, source_clip->name);
      better_timeline_clip_set_selected(source_clip, false);
      better_timeline_clip_set_selected(duplicated_clip, true);
      BLI_insertlinkafter(&track->clips, source_clip, duplicated_clip);
      if (active_duplicate == nullptr) {
        active_duplicate = duplicated_clip;
        active_track = track;
      }
    }
  }

  if (active_duplicate == nullptr || active_track == nullptr) {
    return OPERATOR_CANCELLED;
  }

  sbetter_timeline->selected_clip_index = better_timeline_clip_global_index_from_ptr(
      sbetter_timeline, active_track, active_duplicate);

  if (!better_timeline_clip_drag_start(C, op, event, active_track, active_duplicate, true)) {
    return OPERATOR_CANCELLED;
  }

  auto *drag_data = static_cast<BetterTimelineClipDragData *>(op->customdata);
  drag_data->remove_on_cancel = true;
  ED_area_tag_redraw(area);
  return OPERATOR_RUNNING_MODAL;
}

static void BETTER_TIMELINE_OT_duplicate_clip(wmOperatorType *ot)
{
  ot->name = "Duplicate Better Timeline Clip";
  ot->idname = "BETTER_TIMELINE_OT_duplicate_clip";
  ot->description = "Duplicate selected Better Timeline clips and move the duplicates";

  ot->invoke = better_timeline_duplicate_clip_invoke;
  ot->modal = better_timeline_clip_drag_modal;
  ot->poll = better_timeline_duplicate_clip_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

static bool better_timeline_copy_clip_poll(bContext *C)
{
  return better_timeline_duplicate_clip_poll(C);
}

static wmOperatorStatus better_timeline_copy_clip_exec(bContext *C, wmOperator * /*op*/)
{
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);

  better_timeline_clip_clipboard_clear();
  float anchor_start = FLT_MAX;
  for (const BetterTimelineVisibleRow &row : better_timeline_all_tracks_build(sbetter_timeline)) {
    BetterTimelineTrack *track = row.track;
    if (better_timeline_track_is_group(track)) {
      continue;
    }
    for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first); clip != nullptr;
         clip = clip->next)
    {
      if (!better_timeline_clip_is_selected(clip)) {
        continue;
      }
      if (BetterTimelineClip *clipboard_clip = better_timeline_clip_duplicate(clip)) {
        BLI_addtail(&g_better_timeline_clip_clipboard, clipboard_clip);
        anchor_start = std::min(anchor_start, clip->start_frame);
      }
    }
  }

  if (BLI_listbase_is_empty(&g_better_timeline_clip_clipboard)) {
    return OPERATOR_CANCELLED;
  }

  g_better_timeline_clip_clipboard_anchor_start = (anchor_start == FLT_MAX) ? 0.0f : anchor_start;
  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_copy_clip(wmOperatorType *ot)
{
  ot->name = "Copy Better Timeline Clip";
  ot->idname = "BETTER_TIMELINE_OT_copy_clip";
  ot->description = "Copy selected Better Timeline clips";

  ot->exec = better_timeline_copy_clip_exec;
  ot->poll = better_timeline_copy_clip_poll;

  ot->flag = OPTYPE_INTERNAL;
}

static bool better_timeline_paste_clip_poll(bContext *C)
{
  if (!better_timeline_operator_region_poll(C) || BLI_listbase_is_empty(&g_better_timeline_clip_clipboard)) {
    return false;
  }
  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);
  return better_timeline_has_selected_track(sbetter_timeline) || better_timeline_has_selected_clip(sbetter_timeline);
}

static wmOperatorStatus better_timeline_paste_clip_exec(bContext *C, wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);
  Scene *scene = CTX_data_scene(C);

  BetterTimelineTrack *track = better_timeline_active_selected_track_get(sbetter_timeline);
  if (track == nullptr) {
    BetterTimelineTrack *selected_clip_track = nullptr;
    better_timeline_active_selected_clip_get(sbetter_timeline, &selected_clip_track);
    track = selected_clip_track;
  }
  if (track == nullptr || scene == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const float paste_frame = BKE_scene_frame_get(scene);
  const float frame_delta = paste_frame - g_better_timeline_clip_clipboard_anchor_start;
  Vector<BetterTimelineClip *> pasted_clips;
  Vector<const BetterTimelineClip *> ignored_clips;

  for (BetterTimelineClip *clipboard_clip = static_cast<BetterTimelineClip *>(g_better_timeline_clip_clipboard.first);
       clipboard_clip != nullptr;
       clipboard_clip = clipboard_clip->next)
  {
    if (!ed::better_timeline::track_accepts_clip(*track, *clipboard_clip)) {
      BKE_report(op->reports, RPT_ERROR, "Target track does not accept one or more copied clips");
      return OPERATOR_CANCELLED;
    }
  }

  for (BetterTimelineClip *clipboard_clip = static_cast<BetterTimelineClip *>(g_better_timeline_clip_clipboard.first);
       clipboard_clip != nullptr;
       clipboard_clip = clipboard_clip->next)
  {
    BetterTimelineClip *pasted_clip = better_timeline_clip_duplicate(clipboard_clip);
    if (pasted_clip == nullptr) {
      continue;
    }
    pasted_clip->start_frame = clipboard_clip->start_frame + frame_delta;
    pasted_clip->end_frame = clipboard_clip->end_frame + frame_delta;
    if (!better_timeline_track_can_place_clip(track,
                                              pasted_clip->clip_type,
                                              pasted_clip->start_frame,
                                              pasted_clip->end_frame,
                                              nullptr,
                                              ignored_clips))
    {
      if (pasted_clip->properties != nullptr) {
        IDP_FreeProperty(pasted_clip->properties);
      }
      MEM_delete(pasted_clip);
      BKE_report(op->reports, RPT_ERROR, "Copied clips cannot be pasted at the current frame");
      for (BetterTimelineClip *created_clip : pasted_clips) {
        if (created_clip->properties != nullptr) {
          IDP_FreeProperty(created_clip->properties);
        }
        MEM_delete(created_clip);
      }
      return OPERATOR_CANCELLED;
    }
    better_timeline_clip_assign_duplicate_name(sbetter_timeline, pasted_clip, clipboard_clip->name);
    ignored_clips.append(pasted_clip);
    pasted_clips.append(pasted_clip);
  }

  if (pasted_clips.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  better_timeline_undo_push_init(C, op->type->name);
  better_timeline_clear_selection(sbetter_timeline);
  better_timeline_clear_clip_selection(sbetter_timeline);
  for (BetterTimelineClip *pasted_clip : pasted_clips) {
    BLI_addtail(&track->clips, pasted_clip);
    better_timeline_clip_set_selected(pasted_clip, true);
  }
  sbetter_timeline->selected_clip_index = better_timeline_clip_global_index_from_ptr(
      sbetter_timeline, track, pasted_clips.first());
  better_timeline_tag_space_state_changed(C);
  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_paste_clip(wmOperatorType *ot)
{
  ot->name = "Paste Better Timeline Clip";
  ot->idname = "BETTER_TIMELINE_OT_paste_clip";
  ot->description = "Paste copied Better Timeline clips";

  ot->exec = better_timeline_paste_clip_exec;
  ot->poll = better_timeline_paste_clip_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* -------------------------------------------------------------------- */
/** \name Clip Resize Operator
 * \{ */

static constexpr float BETTER_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH_PX =
    BETTER_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH;

eBetterTimelineClipResizeEdge better_timeline_clip_resize_edge_from_region_position(
    const ARegion *region,
    const SpaceBetterTimeline *sbetter_timeline,
    const BetterTimelineTrack *track,
    const BetterTimelineClip *clip,
    const int region_x,
    const int region_y)
{
  const View2D *v2d = &region->v2d;
  const rctf clip_rect = better_timeline_clip_rect(region, sbetter_timeline, v2d, track, clip);

  if (!BLI_rctf_isect_pt(&clip_rect, float(region_x), float(region_y))) {
    return BETTER_TIMELINE_CLIP_RESIZE_EDGE_NONE;
  }

  const float handle_w = BETTER_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH_PX * UI_SCALE_FAC;
  if (float(region_x) <= clip_rect.xmin + handle_w) {
    return BETTER_TIMELINE_CLIP_RESIZE_EDGE_START;
  }
  if (float(region_x) >= clip_rect.xmax - handle_w) {
    return BETTER_TIMELINE_CLIP_RESIZE_EDGE_END;
  }
  return BETTER_TIMELINE_CLIP_RESIZE_EDGE_NONE;
}

/**
 * Find the clip and resize edge closest to the mouse position, searching all clips in the
 * row rather than relying on the center-distance hit-test. This correctly resolves blend
 * zones where two clips overlap: hovering near the right edge of the left clip and the left
 * edge of the right clip both produce distinct, correct results.
 */
BetterTimelineClip *better_timeline_clip_for_resize_from_region_position(
    const ARegion *region,
    SpaceBetterTimeline *sbetter_timeline,
    const int region_x,
    const int region_y,
    BetterTimelineTrack **r_track,
    eBetterTimelineClipResizeEdge *r_edge)
{
  if (r_track != nullptr) {
    *r_track = nullptr;
  }
  if (r_edge != nullptr) {
    *r_edge = BETTER_TIMELINE_CLIP_RESIZE_EDGE_NONE;
  }

  if (region == nullptr || sbetter_timeline == nullptr ||
      !better_timeline_is_in_timeline_canvas(region, sbetter_timeline, region_x, region_y))
  {
    return nullptr;
  }

  const int row_index = better_timeline_track_from_region_y(region, sbetter_timeline, region_y);
  BetterTimelineTrack *track = better_timeline_visible_row_track_get(sbetter_timeline, row_index);
  if (track == nullptr || better_timeline_track_is_locked(track)) {
    return nullptr;
  }

  const View2D *v2d = &region->v2d;
  const float handle_w = BETTER_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH_PX * UI_SCALE_FAC;

  BetterTimelineClip *best_clip = nullptr;
  eBetterTimelineClipResizeEdge best_edge = BETTER_TIMELINE_CLIP_RESIZE_EDGE_NONE;
  float best_dist = FLT_MAX;

  for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first);
       clip != nullptr;
       clip = clip->next)
  {
    const rctf clip_rect = better_timeline_clip_rect(region, sbetter_timeline, v2d, track, clip);
    /* Vertical bounds must match. */
    if (float(region_y) < clip_rect.ymin || float(region_y) > clip_rect.ymax) {
      continue;
    }

    /* Distance to start edge. */
    const float dist_start = std::abs(float(region_x) - clip_rect.xmin);
    if (dist_start <= handle_w && dist_start < best_dist) {
      best_dist = dist_start;
      best_clip = clip;
      best_edge = BETTER_TIMELINE_CLIP_RESIZE_EDGE_START;
    }

    /* Distance to end edge. */
    const float dist_end = std::abs(float(region_x) - clip_rect.xmax);
    if (dist_end <= handle_w && dist_end < best_dist) {
      best_dist = dist_end;
      best_clip = clip;
      best_edge = BETTER_TIMELINE_CLIP_RESIZE_EDGE_END;
    }
  }

  if (best_clip != nullptr && r_track != nullptr) {
    *r_track = track;
  }
  if (r_edge != nullptr) {
    *r_edge = best_edge;
  }
  return best_clip;
}

static void better_timeline_clip_resize_finish(bContext *C,
                                               wmOperator *op,
                                               const bool apply_changes)
{
  auto *resize_data = static_cast<BetterTimelineClipResizeData *>(op->customdata);
  if (resize_data == nullptr) {
    return;
  }

  if (apply_changes) {
    const bool changed = (resize_data->preview_start_frame != resize_data->initial_start_frame ||
                          resize_data->preview_end_frame != resize_data->initial_end_frame);
    if (changed) {
      better_timeline_undo_push_init(C, op->type->name);
      resize_data->clip->start_frame = resize_data->preview_start_frame;
      resize_data->clip->end_frame = resize_data->preview_end_frame;
      better_timeline_tag_space_state_changed(C);
    }
  }

  better_timeline_clip_resize_visual_state_clear(
      static_cast<SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first));
  WM_cursor_modal_restore(CTX_wm_window(C));
  MEM_delete(resize_data);
  op->customdata = nullptr;
  ED_area_tag_redraw(CTX_wm_area(C));
}

static bool better_timeline_clip_resize_has_changes(const BetterTimelineClipResizeData *resize_data)
{
  if (resize_data == nullptr) {
    return false;
  }

  return resize_data->preview_start_frame != resize_data->initial_start_frame ||
         resize_data->preview_end_frame != resize_data->initial_end_frame;
}

static wmOperatorStatus better_timeline_clip_resize_modal(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent *event)
{
  auto *resize_data = static_cast<BetterTimelineClipResizeData *>(op->customdata);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  auto update_preview = [&]() {
    const float mouse_frame = std::round(
        ui::view2d_region_to_view_x(&region->v2d, event->mval[0]));

    float new_start = resize_data->initial_start_frame;
    float new_end = resize_data->initial_end_frame;

    if (resize_data->speed_scale_mode) {
      /* Future: speed-scale drag for Animation clips (Shift+drag on BETTER_TIMELINE_CT_ANIMATION).
       * When implemented, adjust a speed_scale property on the clip's IDProperty payload
       * instead of trimming the boundary. For now fall through to normal trim. */
    }

    if (resize_data->resize_edge == BETTER_TIMELINE_CLIP_RESIZE_EDGE_START) {
      new_start = std::min(mouse_frame, resize_data->initial_end_frame - 1.0f);
    }
    else {
      new_end = std::max(mouse_frame, resize_data->initial_start_frame + 1.0f);
    }

    if (better_timeline_track_can_place_clip(
            resize_data->track, resize_data->clip->clip_type, new_start, new_end, resize_data->clip))
    {
      resize_data->preview_start_frame = new_start;
      resize_data->preview_end_frame = new_end;
    }

    better_timeline_clip_resize_visual_state_update(sbetter_timeline,
                                                    region,
                                                    resize_data->track,
                                                    resize_data->clip,
                                                    resize_data->preview_start_frame,
                                                    resize_data->preview_end_frame);
  };

  switch (event->type) {
    case MOUSEMOVE:
      update_preview();
      ED_area_tag_redraw(area);
      break;
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        const bool changed = better_timeline_clip_resize_has_changes(resize_data);
        better_timeline_clip_resize_finish(C, op, true);
        return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      }
      break;
    case EVT_RETKEY:
    case EVT_SPACEKEY:
      if (event->val == KM_PRESS) {
        const bool changed = better_timeline_clip_resize_has_changes(resize_data);
        better_timeline_clip_resize_finish(C, op, true);
        return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      }
      break;
    case RIGHTMOUSE:
    case EVT_ESCKEY:
      if (event->type == EVT_ESCKEY || event->val == KM_PRESS) {
        better_timeline_clip_resize_finish(C, op, false);
        return OPERATOR_CANCELLED;
      }
      break;
    default:
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

static bool better_timeline_clip_resize_poll(bContext *C)
{
  return better_timeline_operator_region_poll(C);
}

static wmOperatorStatus better_timeline_clip_resize_invoke(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  if (better_timeline_scrub_event_in_region(area, region, event) ||
      !better_timeline_is_in_timeline_canvas(
          region, sbetter_timeline, event->mval[0], event->mval[1]))
  {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  /* oskey-click is reserved for toggle-select, pass through. */
  if ((event->modifier & KM_OSKEY) != 0) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }
  const bool shift = (event->modifier & KM_SHIFT) != 0;

  /* Use edge-aware pick so blend zones resolve correctly: the clip whose edge is nearest
   * to the mouse wins, regardless of which clip's center is closer. */
  BetterTimelineTrack *track = nullptr;
  eBetterTimelineClipResizeEdge edge = BETTER_TIMELINE_CLIP_RESIZE_EDGE_NONE;
  BetterTimelineClip *clip = better_timeline_clip_for_resize_from_region_position(
      region, sbetter_timeline, event->mval[0], event->mval[1], &track, &edge);

  if (clip == nullptr || track == nullptr || edge == BETTER_TIMELINE_CLIP_RESIZE_EDGE_NONE) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  if (!better_timeline_clip_is_selected(clip)) {
    better_timeline_clear_selection(sbetter_timeline);
    better_timeline_clear_clip_selection(sbetter_timeline);
    better_timeline_clip_set_selected(clip, true);
    sbetter_timeline->selected_clip_index = better_timeline_clip_global_index_from_ptr(
        sbetter_timeline, track, clip);
  }

  auto *resize_data = MEM_new<BetterTimelineClipResizeData>(__func__);
  resize_data->interaction_mode = BETTER_TIMELINE_CLIP_INTERACTION_RESIZE;
  resize_data->track = track;
  resize_data->clip = clip;
  resize_data->resize_edge = edge;
  resize_data->initial_start_frame = clip->start_frame;
  resize_data->initial_end_frame = clip->end_frame;
  resize_data->mouse_start_frame = ui::view2d_region_to_view_x(&region->v2d, event->mval[0]);
  resize_data->preview_start_frame = clip->start_frame;
  resize_data->preview_end_frame = clip->end_frame;
  /* Shift+drag on Animation clips is reserved for future speed-scale mode. */
  resize_data->speed_scale_mode = shift && STREQ(clip->clip_type, "BETTER_TIMELINE_CT_ANIMATION");

  op->customdata = resize_data;

  better_timeline_clip_resize_visual_state_update(
      sbetter_timeline, region, track, clip, clip->start_frame, clip->end_frame);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_X_MOVE);
  WM_event_add_modal_handler(C, op);
  ED_area_tag_redraw(area);
  return OPERATOR_RUNNING_MODAL;
}

static void BETTER_TIMELINE_OT_clip_resize(wmOperatorType *ot)
{
  ot->name = "Resize Better Timeline Clip";
  ot->idname = "BETTER_TIMELINE_OT_clip_resize";
  ot->description =
      "Drag the start or end edge of a clip to resize it. "
      "Shift+drag on Animation clips is reserved for future speed-scale mode";

  ot->invoke = better_timeline_clip_resize_invoke;
  ot->modal = better_timeline_clip_resize_modal;
  ot->poll = better_timeline_clip_resize_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

/** \} */

void better_timeline_clip_ops_register()
{
  WM_operatortype_append(BETTER_TIMELINE_OT_add_clip);
  WM_operatortype_append(BETTER_TIMELINE_OT_clip_select);
  WM_operatortype_append(BETTER_TIMELINE_OT_clip_resize);
  WM_operatortype_append(BETTER_TIMELINE_OT_clip_drag);
  WM_operatortype_append(BETTER_TIMELINE_OT_move_clip);
  WM_operatortype_append(BETTER_TIMELINE_OT_delete_clip);
}

void better_timeline_clipboard_clip_ops_register()
{
  WM_operatortype_append(BETTER_TIMELINE_OT_duplicate_clip);
  WM_operatortype_append(BETTER_TIMELINE_OT_copy_clip);
  WM_operatortype_append(BETTER_TIMELINE_OT_paste_clip);
}

}  // namespace blender
