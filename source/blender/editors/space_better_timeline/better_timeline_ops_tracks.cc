/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#include <algorithm>

#include "BLF_api.hh"

#include "DNA_object_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"

#include "DEG_depsgraph.hh"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "ED_better_timeline.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "better_timeline_intern.hh" /* own include */

namespace blender {

static ListBase g_better_timeline_track_clipboard = {nullptr, nullptr};

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

static void better_timeline_sync_active_track_index(SpaceBetterTimeline *sbetter_timeline,
                                                    const BetterTimelineTrack *active_track)
{
  if (sbetter_timeline == nullptr) {
    return;
  }
  sbetter_timeline->selected_track_index = better_timeline_track_index_from_ptr(sbetter_timeline,
                                                                                active_track);
}

static BetterTimelineTrack *better_timeline_track_with_selected_clip_get(
    SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return nullptr;
  }

  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first); clip != nullptr;
         clip = clip->next)
    {
      if (better_timeline_clip_is_selected(clip)) {
        return track;
      }
    }
  }
  return nullptr;
}

static bool better_timeline_track_object_slot_activate_isect(const ARegion *region,
                                                             const SpaceBetterTimeline *sbetter_timeline,
                                                             const int row_index,
                                                             const BetterTimelineTrack *track,
                                                             const int region_x,
                                                             const int region_y)
{
  if (track == nullptr || track->object == nullptr) {
    return false;
  }

  const rcti slot_rect = better_timeline_track_object_slot_rect(region, sbetter_timeline, row_index);
  if (!BLI_rcti_isect_pt(&slot_rect, region_x, region_y)) {
    return false;
  }

  const rcti picker_rect = better_timeline_track_object_slot_picker_rect(
      region, sbetter_timeline, row_index);
  if (BLI_rcti_isect_pt(&picker_rect, region_x, region_y)) {
    return false;
  }

  const float icon_size = float(UI_ICON_SIZE);
  const int icon_xmin = slot_rect.xmin;
  const int icon_xmax = slot_rect.xmin + int((3.0f * UI_SCALE_FAC) + icon_size + (3.0f * UI_SCALE_FAC));
  const rcti icon_rect{icon_xmin, icon_xmax, slot_rect.ymin, slot_rect.ymax};
  if (BLI_rcti_isect_pt(&icon_rect, region_x, region_y)) {
    return true;
  }

  const char *obj_name = track->object->id.name + 2;
  const float text_x = float(slot_rect.xmin) + (3.0f * UI_SCALE_FAC) + icon_size +
                       (3.0f * UI_SCALE_FAC);
  const int text_width = std::max(0, int(BLF_width(BLF_default(), obj_name, BLF_DRAW_STR_DUMMY_MAX)));
  const int text_xmin = int(text_x);
  const int text_xmax = std::min(text_xmin + text_width, picker_rect.xmin - int(3.0f * UI_SCALE_FAC));
  if (text_xmax <= text_xmin) {
    return false;
  }

  const rcti text_rect{text_xmin, text_xmax, slot_rect.ymin, slot_rect.ymax};
  return BLI_rcti_isect_pt(&text_rect, region_x, region_y);
}

static void better_timeline_track_clipboard_clear()
{
  better_timeline_tracks_free(&g_better_timeline_track_clipboard);
}

static bool better_timeline_selected_tracks_set_muted(SpaceBetterTimeline *sbetter_timeline,
                                                      const bool muted)
{
  bool changed = false;

  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    if (!better_timeline_track_is_selected(track)) {
      continue;
    }

    const bool was_muted = (track->flag & BETTER_TIMELINE_TRACK_MUTED) != 0;
    if (was_muted == muted) {
      continue;
    }

    if (muted) {
      track->flag |= BETTER_TIMELINE_TRACK_MUTED;
    }
    else {
      track->flag &= ~BETTER_TIMELINE_TRACK_MUTED;
    }
    changed = true;
  }

  return changed;
}

static bool better_timeline_selected_tracks_set_locked(SpaceBetterTimeline *sbetter_timeline,
                                                       const bool locked)
{
  bool changed = false;

  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    if (!better_timeline_track_is_selected(track)) {
      continue;
    }

    const bool was_locked = (track->flag & BETTER_TIMELINE_TRACK_LOCKED) != 0;
    if (was_locked == locked) {
      continue;
    }

    if (locked) {
      track->flag |= BETTER_TIMELINE_TRACK_LOCKED;
      better_timeline_clear_clip_selection_for_track(sbetter_timeline, track);
    }
    else {
      track->flag &= ~BETTER_TIMELINE_TRACK_LOCKED;
    }
    changed = true;
  }

  return changed;
}

static bool better_timeline_track_select_poll(bContext *C)
{
  return better_timeline_operator_region_poll(C);
}

wmOperatorStatus better_timeline_track_select_click_invoke(bContext *C, const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  if (better_timeline_scrub_event_in_region(area, region, event)) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }
  if (better_timeline_clip_from_region_position(
          region, sbetter_timeline, event->mval[0], event->mval[1], nullptr) != nullptr)
  {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }
  if (better_timeline_is_in_add_button(region, sbetter_timeline, event->mval[0], event->mval[1])) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }
  if (better_timeline_is_on_panel_divider(region, sbetter_timeline, event->mval[0])) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  /* Object slot click area. */
  if (better_timeline_is_in_track_list_pane(region, sbetter_timeline, event->mval[0])) {
    const int obj_row = better_timeline_track_from_region_y(
        region, sbetter_timeline, event->mval[1]);
    if (obj_row >= 0) {
      BetterTimelineTrack *obj_track = better_timeline_track_at_index(sbetter_timeline, obj_row);
      if (obj_track != nullptr) {
        const ed::better_timeline::BetterTimelineTrackType *tt =
            ed::better_timeline::track_type_find_from_idname(obj_track->track_type);
        if (tt != nullptr && tt->has_object_slot) {
          const rcti picker_rect = better_timeline_track_object_slot_picker_rect(
              region, sbetter_timeline, obj_row);
          if (BLI_rcti_isect_pt(&picker_rect, event->mval[0], event->mval[1])) {
            wmOperatorType *ot = WM_operatortype_find(
                "BETTER_TIMELINE_OT_track_pick_object", true);
            if (ot != nullptr) {
              PointerRNA op_props = WM_operator_properties_create_ptr(ot);
              RNA_int_set(&op_props, "track_index", obj_row);
              WM_operator_name_call_ptr(
                  C, ot, wm::OpCallContext::InvokeDefault, &op_props, event);
              WM_operator_properties_free(&op_props);
            }
            return OPERATOR_FINISHED;
          }

          if (better_timeline_track_object_slot_activate_isect(
                  region, sbetter_timeline, obj_row, obj_track, event->mval[0], event->mval[1]))
          {
            Scene *scene = CTX_data_scene(C);
            ViewLayer *view_layer = CTX_data_view_layer(C);
            BKE_view_layer_synced_ensure(scene, view_layer);
            Base *base = BKE_view_layer_base_find(view_layer, obj_track->object);
            if (base != nullptr) {
              for (Base &b : view_layer->object_bases) {
                ed::object::base_select(&b, ed::object::BA_DESELECT);
              }
              ed::object::base_select(base, ed::object::BA_SELECT);
              ed::object::base_activate(C, base);
              DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
              WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
              WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
              ED_area_tag_redraw(area);
            }
            /* This click only targets the bound scene object. It should not complete the Better
             * Timeline track-selection operator path or trigger Better Timeline undo snapshotting. */
            return OPERATOR_CANCELLED;
          }
        }
      }
    }
  }

  /* Mute button click: toggle track muted flag. */
  {
    const int mute_row = better_timeline_track_from_mute_button_region_pos(
        region, sbetter_timeline, event->mval[0], event->mval[1]);
    if (mute_row >= 0) {
      BetterTimelineTrack *track = better_timeline_track_at_index(sbetter_timeline, mute_row);
      if (track != nullptr) {
        better_timeline_undo_push_init(C, "Toggle Track Mute");
        if (track->flag & BETTER_TIMELINE_TRACK_MUTED) {
          track->flag &= ~BETTER_TIMELINE_TRACK_MUTED;
        }
        else {
          track->flag |= BETTER_TIMELINE_TRACK_MUTED;
        }
        ED_area_tag_redraw(area);
        return OPERATOR_FINISHED;
      }
    }
  }

  /* Lock button click: toggle track locked flag. */
  {
    const int lock_row = better_timeline_track_from_lock_button_region_pos(
        region, sbetter_timeline, event->mval[0], event->mval[1]);
    if (lock_row >= 0) {
      BetterTimelineTrack *track = better_timeline_track_at_index(sbetter_timeline, lock_row);
      if (track != nullptr) {
        better_timeline_undo_push_init(C, "Toggle Track Lock");
        if (track->flag & BETTER_TIMELINE_TRACK_LOCKED) {
          track->flag &= ~BETTER_TIMELINE_TRACK_LOCKED;
        }
        else {
          track->flag |= BETTER_TIMELINE_TRACK_LOCKED;
          better_timeline_clear_clip_selection_for_track(sbetter_timeline, track);
        }
        ED_area_tag_redraw(area);
        return OPERATOR_FINISHED;
      }
    }
  }

  const int clicked_track_index = better_timeline_track_from_region_y(
      region, sbetter_timeline, event->mval[1]);
  const bool shift = (event->modifier & KM_SHIFT) != 0;
  const bool oskey = (event->modifier & KM_OSKEY) != 0;
  const bool allow_track_reorder_gesture = !shift && !oskey;

  if (clicked_track_index < 0) {
    if (!shift && !oskey) {
      if (!better_timeline_is_in_track_list_pane(region, sbetter_timeline, event->mval[0]) &&
          better_timeline_has_selected_clip(sbetter_timeline))
      {
        better_timeline_clear_clip_selection(sbetter_timeline);
        ED_area_tag_redraw(area);
        return OPERATOR_FINISHED;
      }
      if (better_timeline_has_selected_track(sbetter_timeline)) {
        better_timeline_clear_selection(sbetter_timeline);
        better_timeline_clear_clip_selection(sbetter_timeline);
        ED_area_tag_redraw(area);
        return OPERATOR_FINISHED;
      }
    }
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  BetterTimelineTrack *clicked_track = better_timeline_track_at_index(sbetter_timeline,
                                                                      clicked_track_index);
  if (clicked_track == nullptr) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  const bool clicked_in_track_list = better_timeline_is_in_track_list_pane(
      region, sbetter_timeline, event->mval[0]);

  if (shift) {
    int anchor_index = sbetter_timeline->selected_track_index;
    if (anchor_index < 0 || anchor_index >= better_timeline_track_count(sbetter_timeline)) {
      anchor_index = better_timeline_first_selected_track_index(sbetter_timeline);
    }
    if (anchor_index < 0) {
      anchor_index = clicked_track_index;
    }

    const int min_index = std::min(anchor_index, clicked_track_index);
    const int max_index = std::max(anchor_index, clicked_track_index);
    for (int track_index = min_index; track_index <= max_index; track_index++) {
      better_timeline_track_set_selected(
          better_timeline_track_at_index(sbetter_timeline, track_index), true);
    }
    sbetter_timeline->selected_track_index = clicked_track_index;
  }
  else if (oskey) {
    const bool new_selected_state = !better_timeline_track_is_selected(clicked_track);
    better_timeline_track_set_selected(clicked_track, new_selected_state);
    if (new_selected_state) {
      sbetter_timeline->selected_track_index = clicked_track_index;
    }
    else if (sbetter_timeline->selected_track_index == clicked_track_index) {
      sbetter_timeline->selected_track_index = better_timeline_first_selected_track_index(
          sbetter_timeline);
    }
  }
  else {
    const bool clicked_track_selected = better_timeline_track_is_selected(clicked_track);
    const int selected_track_count = better_timeline_selected_track_count(sbetter_timeline);

    const bool already_only_selected = better_timeline_track_is_selected(clicked_track) &&
                                       selected_track_count == 1 &&
                                       sbetter_timeline->selected_track_index == clicked_track_index &&
                                       !better_timeline_has_selected_clip(sbetter_timeline);
    if (already_only_selected) {
      return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
    }

    if (clicked_in_track_list && clicked_track_selected && selected_track_count > 1) {
      return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
    }

    better_timeline_select_only_track(sbetter_timeline, clicked_track_index);
    better_timeline_clear_clip_selection(sbetter_timeline);
  }

  ED_area_tag_redraw(area);

  if (clicked_in_track_list && allow_track_reorder_gesture) {
    return OPERATOR_FINISHED | OPERATOR_PASS_THROUGH;
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus better_timeline_track_select_invoke(bContext *C,
                                                            wmOperator * /*op*/,
                                                            const wmEvent *event)
{
  return better_timeline_track_select_click_invoke(C, event);
}

static void BETTER_TIMELINE_OT_track_select(wmOperatorType *ot)
{
  ot->name = "Select Better Timeline Track";
  ot->idname = "BETTER_TIMELINE_OT_track_select";
  ot->description = "Select a track row in Better Timeline";

  ot->invoke = better_timeline_track_select_invoke;
  ot->poll = better_timeline_track_select_poll;

  ot->flag = OPTYPE_INTERNAL;
}

static bool better_timeline_track_reorder_poll(bContext *C)
{
  return better_timeline_track_select_poll(C);
}

static void better_timeline_track_reorder_finish(bContext *C, wmOperator *op)
{
  auto *reorder_data = static_cast<BetterTimelineTrackReorderData *>(op->customdata);
  if (reorder_data == nullptr) {
    return;
  }

  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *win = CTX_wm_window(C);
  if (reorder_data->autoscroll_timer != nullptr) {
    WM_event_timer_remove(wm, win, reorder_data->autoscroll_timer);
    reorder_data->autoscroll_timer = nullptr;
  }

  better_timeline_track_drag_visual_state_clear(
      static_cast<SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first));
  WM_cursor_modal_restore(win);
  MEM_delete(reorder_data);
  op->customdata = nullptr;
}

static wmOperatorStatus better_timeline_track_reorder_modal(bContext *C,
                                                            wmOperator *op,
                                                            const wmEvent *event)
{
  auto *reorder_data = static_cast<BetterTimelineTrackReorderData *>(op->customdata);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  switch (event->type) {
    case MOUSEMOVE: {
      const int track_count = better_timeline_track_count(sbetter_timeline);
      if (track_count < 2 ||
          better_timeline_track_index_from_ptr(sbetter_timeline, reorder_data->dragged_track) < 0)
      {
        break;
      }

      reorder_data->last_mouse_y = event->mval[1];
      better_timeline_track_reorder_autoscroll_apply(
          region, sbetter_timeline, reorder_data->last_mouse_y);
      reorder_data->current_insertion_index = better_timeline_track_insertion_index_from_region_y(
          region, sbetter_timeline, reorder_data->last_mouse_y);
      better_timeline_track_drag_visual_state_update(
          sbetter_timeline, region, reorder_data->dragged_track, reorder_data->current_insertion_index);
      ED_area_tag_redraw(area);
      break;
    }
    case TIMER:
      if (event->customdata == reorder_data->autoscroll_timer) {
        const int track_count = better_timeline_track_count(sbetter_timeline);
        if (track_count < 2 || !better_timeline_track_reorder_autoscroll_apply(
                                   region, sbetter_timeline, reorder_data->last_mouse_y))
        {
          break;
        }

        reorder_data->current_insertion_index = better_timeline_track_insertion_index_from_region_y(
            region, sbetter_timeline, reorder_data->last_mouse_y);
        better_timeline_track_drag_visual_state_update(
            sbetter_timeline,
            region,
            reorder_data->dragged_track,
            reorder_data->current_insertion_index);
        ED_area_tag_redraw(area);
      }
      break;
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        if (better_timeline_track_index_from_ptr(sbetter_timeline, reorder_data->dragged_track) >= 0)
        {
          const bool changed = better_timeline_reorder_selected_tracks_would_change(
              sbetter_timeline, reorder_data->current_insertion_index);
          if (changed) {
            better_timeline_undo_push_init(C, op->type->name);
            better_timeline_reorder_selected_tracks_to_insertion_index(
                sbetter_timeline, reorder_data->current_insertion_index);
            better_timeline_tag_space_state_changed(C);
          }
        }
        better_timeline_sync_active_track_index(sbetter_timeline, reorder_data->active_track);
        better_timeline_track_reorder_finish(C, op);
        ED_area_tag_redraw(area);
        return OPERATOR_FINISHED;
      }
      break;
    case EVT_ESCKEY:
      better_timeline_sync_active_track_index(sbetter_timeline, reorder_data->active_track);
      better_timeline_track_reorder_finish(C, op);
      ED_area_tag_redraw(area);
      return OPERATOR_CANCELLED;
    default:
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus better_timeline_track_reorder_invoke(bContext *C,
                                                             wmOperator *op,
                                                             const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  int drag_start_mval[2];
  WM_event_drag_start_mval(event, region, drag_start_mval);

  if (better_timeline_scrub_event_in_region(area, region, event) ||
      better_timeline_clip_from_region_position(
          region, sbetter_timeline, drag_start_mval[0], drag_start_mval[1], nullptr) != nullptr ||
      better_timeline_is_in_add_button(
          region, sbetter_timeline, drag_start_mval[0], drag_start_mval[1]) ||
      better_timeline_is_on_panel_divider(region, sbetter_timeline, drag_start_mval[0]) ||
      better_timeline_is_in_track_scrollbar(
          region, sbetter_timeline, drag_start_mval[0], drag_start_mval[1]) ||
      !better_timeline_is_in_track_list_pane(region, sbetter_timeline, drag_start_mval[0]))
  {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  const int dragged_track_index = better_timeline_track_from_region_y(
      region, sbetter_timeline, drag_start_mval[1]);
  BetterTimelineTrack *dragged_track = better_timeline_track_at_index(sbetter_timeline,
                                                                      dragged_track_index);
  if (dragged_track == nullptr || better_timeline_track_count(sbetter_timeline) < 2) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  if (!better_timeline_track_is_selected(dragged_track)) {
    better_timeline_select_only_track(sbetter_timeline, dragged_track_index);
  }

  BetterTimelineTrack *active_track = better_timeline_track_at_index(
      sbetter_timeline, sbetter_timeline->selected_track_index);
  if (active_track == nullptr) {
    active_track = dragged_track;
    better_timeline_sync_active_track_index(sbetter_timeline, active_track);
  }

  auto *reorder_data = MEM_new<BetterTimelineTrackReorderData>(__func__);
  reorder_data->dragged_track = dragged_track;
  reorder_data->active_track = active_track;
  reorder_data->current_insertion_index = better_timeline_track_insertion_index_from_region_y(
      region, sbetter_timeline, drag_start_mval[1]);
  reorder_data->last_mouse_y = drag_start_mval[1];
  reorder_data->autoscroll_timer = WM_event_timer_add(
      CTX_wm_manager(C), CTX_wm_window(C), TIMER, BETTER_TIMELINE_REORDER_AUTOSCROLL_TIMER_STEP);
  op->customdata = reorder_data;

  better_timeline_track_drag_visual_state_update(
      sbetter_timeline, region, dragged_track, reorder_data->current_insertion_index);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_Y_MOVE);
  WM_event_add_modal_handler(C, op);
  ED_area_tag_redraw(area);
  return OPERATOR_RUNNING_MODAL;
}

static void BETTER_TIMELINE_OT_track_reorder(wmOperatorType *ot)
{
  ot->name = "Reorder Better Timeline Track";
  ot->idname = "BETTER_TIMELINE_OT_track_reorder";
  ot->description = "Drag a Better Timeline track row to reorder it";

  ot->invoke = better_timeline_track_reorder_invoke;
  ot->modal = better_timeline_track_reorder_modal;
  ot->poll = better_timeline_track_reorder_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

static bool better_timeline_add_track_poll(bContext *C)
{
  return better_timeline_track_select_poll(C);
}

static wmOperatorStatus better_timeline_add_track_exec(bContext *C, wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  char track_type_idname[ed::better_timeline::BETTER_TIMELINE_TYPE_IDNAME_MAX];
  RNA_string_get(op->ptr, "track_type", track_type_idname);

  const ed::better_timeline::BetterTimelineTrackType *track_type =
      (track_type_idname[0] != '\0') ?
          ed::better_timeline::track_type_find_from_idname(track_type_idname) :
          ed::better_timeline::default_track_type_get();

  if (track_type == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No Better Timeline track type is available");
    return OPERATOR_CANCELLED;
  }

  better_timeline_undo_push_init(C, op->type->name);
  BetterTimelineTrack *track = better_timeline_track_create(track_type->idname,
                                                            sbetter_timeline->next_track_name_index);
  BLI_addtail(&sbetter_timeline->tracks, track);
  sbetter_timeline->next_track_name_index = std::max(1, sbetter_timeline->next_track_name_index + 1);
  better_timeline_select_only_track(sbetter_timeline,
                                    better_timeline_track_count(sbetter_timeline) - 1);
  sbetter_timeline->track_scroll_offset = better_timeline_track_scroll_max(region, sbetter_timeline);
  better_timeline_tag_space_state_changed(C);
  ED_area_tag_redraw(area);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus better_timeline_add_track_invoke(bContext *C,
                                                         wmOperator *op,
                                                         const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  if (!better_timeline_is_in_add_button(region, sbetter_timeline, event->mval[0], event->mval[1])) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  ui::PopupMenu *pup = ui::popup_menu_begin(C, "Add Track", ICON_NONE);
  ui::Layout &layout = *popup_menu_layout(pup);
  ed::better_timeline::foreach_track_type(
      [&](const ed::better_timeline::BetterTimelineTrackType &track_type) {
        PointerRNA op_ptr = layout.op("BETTER_TIMELINE_OT_add_track",
                                      track_type.label[0] != '\0' ? track_type.label :
                                                                    track_type.idname,
                                      ICON_NONE);
        RNA_string_set(&op_ptr, "track_type", track_type.idname);
      });
  popup_menu_end(C, pup);
  UNUSED_VARS(op);
  return OPERATOR_INTERFACE;
}

static void BETTER_TIMELINE_OT_add_track(wmOperatorType *ot)
{
  ot->name = "Add Better Timeline Track";
  ot->idname = "BETTER_TIMELINE_OT_add_track";
  ot->description = "Add a new track to Better Timeline";

  ot->invoke = better_timeline_add_track_invoke;
  ot->exec = better_timeline_add_track_exec;
  ot->poll = better_timeline_add_track_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_string(ot->srna,
                 "track_type",
                 nullptr,
                 ed::better_timeline::BETTER_TIMELINE_TYPE_IDNAME_MAX,
                 "Track Type",
                 "Registered Better Timeline track type to create");
}

static bool better_timeline_add_track_menu_poll(bContext *C)
{
  return better_timeline_track_select_poll(C);
}

static wmOperatorStatus better_timeline_add_track_menu_invoke(bContext *C,
                                                              wmOperator * /*op*/,
                                                              const wmEvent * /*event*/)
{
  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);
  if (better_timeline_has_selected_clip(sbetter_timeline)) {
    const BetterTimelineTrack *track = better_timeline_track_with_selected_clip_get(
        const_cast<SpaceBetterTimeline *>(sbetter_timeline));
    if (track == nullptr || better_timeline_track_is_locked(track)) {
      return OPERATOR_CANCELLED;
    }

    const ed::better_timeline::BetterTimelineTrackType *track_type =
        ed::better_timeline::track_type_find_from_idname(track->track_type);
    if (track_type == nullptr) {
      return OPERATOR_CANCELLED;
    }

    const Vector<const ed::better_timeline::BetterTimelineClipType *> clip_types =
        ed::better_timeline::compatible_clip_types(*track_type);
    if (clip_types.is_empty()) {
      return OPERATOR_CANCELLED;
    }

    ui::PopupMenu *pup = ui::popup_menu_begin(C, "Add Clip", ICON_NONE);
    ui::Layout &layout = *popup_menu_layout(pup);
    for (const ed::better_timeline::BetterTimelineClipType *clip_type : clip_types) {
      PointerRNA op_ptr = layout.op("BETTER_TIMELINE_OT_add_clip",
                                    clip_type->label[0] != '\0' ? clip_type->label :
                                                                  clip_type->idname,
                                    ICON_NONE);
      RNA_string_set(&op_ptr, "clip_type", clip_type->idname);
    }
    popup_menu_end(C, pup);
    return OPERATOR_INTERFACE;
  }

  if (better_timeline_has_selected_track(sbetter_timeline)) {
    const BetterTimelineTrack *track = better_timeline_track_at_index(
        sbetter_timeline, sbetter_timeline->selected_track_index);
    if (track == nullptr || !better_timeline_track_is_selected(track)) {
      track = better_timeline_track_at_index(sbetter_timeline,
                                             better_timeline_first_selected_track_index(
                                                 sbetter_timeline));
    }
    if (track == nullptr || better_timeline_track_is_locked(track)) {
      return OPERATOR_CANCELLED;
    }

    const ed::better_timeline::BetterTimelineTrackType *track_type =
        ed::better_timeline::track_type_find_from_idname(track->track_type);
    if (track_type == nullptr) {
      return OPERATOR_CANCELLED;
    }

    const Vector<const ed::better_timeline::BetterTimelineClipType *> clip_types =
        ed::better_timeline::compatible_clip_types(*track_type);
    if (clip_types.is_empty()) {
      return OPERATOR_CANCELLED;
    }

    ui::PopupMenu *pup = ui::popup_menu_begin(C, "Add Clip", ICON_NONE);
    ui::Layout &layout = *popup_menu_layout(pup);
    for (const ed::better_timeline::BetterTimelineClipType *clip_type : clip_types) {
      PointerRNA op_ptr = layout.op("BETTER_TIMELINE_OT_add_clip",
                                    clip_type->label[0] != '\0' ? clip_type->label :
                                                                  clip_type->idname,
                                    ICON_NONE);
      RNA_string_set(&op_ptr, "clip_type", clip_type->idname);
    }
    popup_menu_end(C, pup);
    return OPERATOR_INTERFACE;
  }

  ui::PopupMenu *pup = ui::popup_menu_begin(C, "Add Track", ICON_NONE);
  ui::Layout &layout = *popup_menu_layout(pup);
  ed::better_timeline::foreach_track_type(
      [&](const ed::better_timeline::BetterTimelineTrackType &track_type) {
        PointerRNA op_ptr = layout.op("BETTER_TIMELINE_OT_add_track",
                                      track_type.label[0] != '\0' ? track_type.label :
                                                                    track_type.idname,
                                      ICON_NONE);
        RNA_string_set(&op_ptr, "track_type", track_type.idname);
      });
  popup_menu_end(C, pup);
  return OPERATOR_INTERFACE;
}

static void BETTER_TIMELINE_OT_add_track_menu(wmOperatorType *ot)
{
  ot->name = "Add Better Timeline Track Menu";
  ot->idname = "BETTER_TIMELINE_OT_add_track_menu";
  ot->description = "Choose a track type to add to Better Timeline";

  ot->invoke = better_timeline_add_track_menu_invoke;
  ot->poll = better_timeline_add_track_menu_poll;

  ot->flag = OPTYPE_INTERNAL;
}

static bool better_timeline_duplicate_track_poll(bContext *C)
{
  if (!better_timeline_track_select_poll(C)) {
    return false;
  }
  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);
  return !better_timeline_has_selected_clip(sbetter_timeline) &&
         better_timeline_has_selected_track(sbetter_timeline);
}

static wmOperatorStatus better_timeline_duplicate_track_exec(bContext *C, wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  Vector<BetterTimelineTrack *> selected_tracks;
  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    if (better_timeline_track_is_selected(track)) {
      selected_tracks.append(track);
    }
  }
  if (selected_tracks.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  better_timeline_undo_push_init(C, op->type->name);
  better_timeline_clear_selection(sbetter_timeline);
  better_timeline_clear_clip_selection(sbetter_timeline);

  BetterTimelineTrack *last_duplicate = nullptr;
  for (BetterTimelineTrack *source_track : selected_tracks) {
    BetterTimelineTrack *duplicated_track = better_timeline_track_duplicate(source_track);
    if (duplicated_track == nullptr) {
      continue;
    }
    better_timeline_track_assign_duplicate_name(sbetter_timeline, duplicated_track, source_track->name);
    better_timeline_track_set_selected(duplicated_track, true);
    BLI_insertlinkafter(&sbetter_timeline->tracks, source_track, duplicated_track);
    last_duplicate = duplicated_track;
  }

  if (last_duplicate == nullptr) {
    return OPERATOR_CANCELLED;
  }

  better_timeline_sync_active_track_index(sbetter_timeline, last_duplicate);
  better_timeline_tag_space_state_changed(C);
  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_duplicate_track(wmOperatorType *ot)
{
  ot->name = "Duplicate Better Timeline Track";
  ot->idname = "BETTER_TIMELINE_OT_duplicate_track";
  ot->description = "Duplicate selected Better Timeline tracks below the originals";

  ot->exec = better_timeline_duplicate_track_exec;
  ot->poll = better_timeline_duplicate_track_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static bool better_timeline_copy_track_poll(bContext *C)
{
  return better_timeline_duplicate_track_poll(C);
}

static wmOperatorStatus better_timeline_copy_track_exec(bContext *C, wmOperator * /*op*/)
{
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);
  better_timeline_track_clipboard_clear();

  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    if (!better_timeline_track_is_selected(track)) {
      continue;
    }
    if (BetterTimelineTrack *clipboard_track = better_timeline_track_duplicate(track)) {
      BLI_addtail(&g_better_timeline_track_clipboard, clipboard_track);
    }
  }

  return BLI_listbase_is_empty(&g_better_timeline_track_clipboard) ? OPERATOR_CANCELLED :
                                                                     OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_copy_track(wmOperatorType *ot)
{
  ot->name = "Copy Better Timeline Track";
  ot->idname = "BETTER_TIMELINE_OT_copy_track";
  ot->description = "Copy selected Better Timeline tracks";

  ot->exec = better_timeline_copy_track_exec;
  ot->poll = better_timeline_copy_track_poll;

  ot->flag = OPTYPE_INTERNAL;
}

static bool better_timeline_paste_track_poll(bContext *C)
{
  if (!better_timeline_track_select_poll(C) || BLI_listbase_is_empty(&g_better_timeline_track_clipboard)) {
    return false;
  }
  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);
  return !better_timeline_has_selected_clip(sbetter_timeline);
}

static wmOperatorStatus better_timeline_paste_track_exec(bContext *C, wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);
  BetterTimelineTrack *insert_after = better_timeline_track_at_index(
      sbetter_timeline, sbetter_timeline->selected_track_index);
  if (insert_after == nullptr) {
    insert_after = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.last);
  }

  better_timeline_undo_push_init(C, op->type->name);
  better_timeline_clear_selection(sbetter_timeline);
  better_timeline_clear_clip_selection(sbetter_timeline);

  BetterTimelineTrack *last_inserted = insert_after;
  BetterTimelineTrack *last_pasted_track = nullptr;
  for (BetterTimelineTrack *clipboard_track = static_cast<BetterTimelineTrack *>(g_better_timeline_track_clipboard.first);
       clipboard_track != nullptr;
       clipboard_track = clipboard_track->next)
  {
    BetterTimelineTrack *pasted_track = better_timeline_track_duplicate(clipboard_track);
    if (pasted_track == nullptr) {
      continue;
    }
    better_timeline_track_assign_duplicate_name(sbetter_timeline, pasted_track, clipboard_track->name);
    better_timeline_track_set_selected(pasted_track, true);
    if (last_inserted != nullptr) {
      BLI_insertlinkafter(&sbetter_timeline->tracks, last_inserted, pasted_track);
    }
    else {
      BLI_addhead(&sbetter_timeline->tracks, pasted_track);
    }
    last_inserted = pasted_track;
    last_pasted_track = pasted_track;
  }

  if (last_pasted_track == nullptr) {
    return OPERATOR_CANCELLED;
  }

  better_timeline_sync_active_track_index(sbetter_timeline, last_pasted_track);
  better_timeline_tag_space_state_changed(C);
  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_paste_track(wmOperatorType *ot)
{
  ot->name = "Paste Better Timeline Track";
  ot->idname = "BETTER_TIMELINE_OT_paste_track";
  ot->description = "Paste copied Better Timeline tracks";

  ot->exec = better_timeline_paste_track_exec;
  ot->poll = better_timeline_paste_track_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static bool better_timeline_track_requires_delete_confirm(
    const SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return false;
  }

  for (const BetterTimelineTrack *track = static_cast<const BetterTimelineTrack *>(
           sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    if (better_timeline_track_is_selected(track) && !BLI_listbase_is_empty(&track->clips)) {
      return true;
    }
  }
  return false;
}

static wmOperatorStatus better_timeline_delete_track_exec(bContext *C, wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  if (!better_timeline_has_selected_track(sbetter_timeline)) {
    return OPERATOR_CANCELLED;
  }

  better_timeline_undo_push_init(C, op->type->name);
  BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
  while (track != nullptr) {
    BetterTimelineTrack *track_next = track->next;
    if (better_timeline_track_is_selected(track)) {
      BLI_remlink(&sbetter_timeline->tracks, track);
      better_timeline_track_free(track);
    }
    track = track_next;
  }

  const int track_count = better_timeline_track_count(sbetter_timeline);
  if (track_count == 0) {
    sbetter_timeline->selected_track_index = -1;
  }
  else {
    sbetter_timeline->selected_track_index = -1;
  }
  sbetter_timeline->track_scroll_offset = better_timeline_track_scroll_offset(region, sbetter_timeline);
  better_timeline_tag_space_state_changed(C);
  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus better_timeline_delete_track_invoke(bContext *C,
                                                            wmOperator *op,
                                                            const wmEvent * /*event*/)
{
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);

  if (better_timeline_track_requires_delete_confirm(sbetter_timeline) &&
      RNA_boolean_get(op->ptr, "confirm"))
  {
    return WM_operator_confirm_ex(C,
                                  op,
                                  "Delete selected track?",
                                  nullptr,
                                  "Delete",
                                  ui::AlertIcon::None,
                                  false);
  }

  return better_timeline_delete_track_exec(C, op);
}

static bool better_timeline_delete_track_poll(bContext *C)
{
  if (!better_timeline_track_select_poll(C)) {
    return false;
  }

  const ScrArea *area = CTX_wm_area(C);
  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(area->spacedata.first);
  return better_timeline_has_selected_track(sbetter_timeline);
}

static void BETTER_TIMELINE_OT_delete_track(wmOperatorType *ot)
{
  ot->name = "Delete Better Timeline Track";
  ot->idname = "BETTER_TIMELINE_OT_delete_track";
  ot->description = "Delete the selected track from Better Timeline";

  ot->invoke = better_timeline_delete_track_invoke;
  ot->exec = better_timeline_delete_track_exec;
  ot->poll = better_timeline_delete_track_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  WM_operator_properties_confirm_or_exec(ot);
}

static bool better_timeline_toggle_selected_tracks_poll(bContext *C)
{
  if (!better_timeline_track_select_poll(C)) {
    return false;
  }

  const ScrArea *area = CTX_wm_area(C);
  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(area->spacedata.first);
  return better_timeline_has_selected_track(sbetter_timeline);
}

static wmOperatorStatus better_timeline_toggle_selected_tracks_mute_exec(bContext *C,
                                                                         wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  bool should_mute = false;
  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    if (better_timeline_track_is_selected(track) && !better_timeline_track_is_muted(track)) {
      should_mute = true;
      break;
    }
  }

  better_timeline_undo_push_init(C, op->type->name);
  if (!better_timeline_selected_tracks_set_muted(sbetter_timeline, should_mute)) {
    return OPERATOR_CANCELLED;
  }

  better_timeline_tag_space_state_changed(C);
  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_toggle_selected_tracks_mute(wmOperatorType *ot)
{
  ot->name = "Toggle Better Timeline Track Mute";
  ot->idname = "BETTER_TIMELINE_OT_toggle_selected_tracks_mute";
  ot->description = "Mute or unmute the selected Better Timeline tracks";

  ot->exec = better_timeline_toggle_selected_tracks_mute_exec;
  ot->poll = better_timeline_toggle_selected_tracks_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus better_timeline_toggle_selected_tracks_lock_exec(bContext *C,
                                                                         wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  bool should_lock = false;
  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    if (better_timeline_track_is_selected(track) && !better_timeline_track_is_locked(track)) {
      should_lock = true;
      break;
    }
  }

  better_timeline_undo_push_init(C, op->type->name);
  if (!better_timeline_selected_tracks_set_locked(sbetter_timeline, should_lock)) {
    return OPERATOR_CANCELLED;
  }

  better_timeline_tag_space_state_changed(C);
  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_toggle_selected_tracks_lock(wmOperatorType *ot)
{
  ot->name = "Toggle Better Timeline Track Lock";
  ot->idname = "BETTER_TIMELINE_OT_toggle_selected_tracks_lock";
  ot->description = "Lock or unlock the selected Better Timeline tracks";

  ot->exec = better_timeline_toggle_selected_tracks_lock_exec;
  ot->poll = better_timeline_toggle_selected_tracks_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static bool better_timeline_clear_selection_poll(bContext *C)
{
  if (!better_timeline_track_select_poll(C)) {
    return false;
  }

  const ScrArea *area = CTX_wm_area(C);
  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(area->spacedata.first);
  return better_timeline_has_selected_track(sbetter_timeline) ||
         better_timeline_has_selected_clip(sbetter_timeline);
}

static wmOperatorStatus better_timeline_clear_selection_exec(bContext *C, wmOperator * /*op*/)
{
  ScrArea *area = CTX_wm_area(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  better_timeline_clear_selection(sbetter_timeline);
  better_timeline_clear_clip_selection(sbetter_timeline);
  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_clear_selection(wmOperatorType *ot)
{
  ot->name = "Clear Better Timeline Selection";
  ot->idname = "BETTER_TIMELINE_OT_clear_selection";
  ot->description = "Clear selected tracks and clips in Better Timeline";

  ot->exec = better_timeline_clear_selection_exec;
  ot->poll = better_timeline_clear_selection_poll;

  ot->flag = OPTYPE_INTERNAL;
}

static void BETTER_TIMELINE_OT_track_drop_object(wmOperatorType *ot);
static void BETTER_TIMELINE_OT_track_pick_object(wmOperatorType *ot);

void better_timeline_track_ops_register()
{
  WM_operatortype_append(BETTER_TIMELINE_OT_add_track);
  WM_operatortype_append(BETTER_TIMELINE_OT_add_track_menu);
  WM_operatortype_append(BETTER_TIMELINE_OT_delete_track);
  WM_operatortype_append(BETTER_TIMELINE_OT_toggle_selected_tracks_mute);
  WM_operatortype_append(BETTER_TIMELINE_OT_toggle_selected_tracks_lock);
  WM_operatortype_append(BETTER_TIMELINE_OT_clear_selection);
  WM_operatortype_append(BETTER_TIMELINE_OT_track_select);
  WM_operatortype_append(BETTER_TIMELINE_OT_track_reorder);
  /* Drop operator must be registered at startup alongside other operators. */
  WM_operatortype_append(BETTER_TIMELINE_OT_track_drop_object);
  WM_operatortype_append(BETTER_TIMELINE_OT_track_pick_object);
}

void better_timeline_clipboard_track_ops_register()
{
  WM_operatortype_append(BETTER_TIMELINE_OT_duplicate_track);
  WM_operatortype_append(BETTER_TIMELINE_OT_copy_track);
  WM_operatortype_append(BETTER_TIMELINE_OT_paste_track);
}

/* -------------------------------------------------------------------- */
/** \name Drop Object onto Track
 * \{ */

static bool better_timeline_track_drop_object_poll(bContext *C,
                                                   wmDrag *drag,
                                                   const wmEvent *event)
{
  if (!WM_drag_is_ID_type(drag, ID_OB)) {
    return false;
  }
  const ARegion *region = CTX_wm_region(C);
  const ScrArea *area = CTX_wm_area(C);
  if (region == nullptr || area == nullptr || area->spacetype != SPACE_BETTER_TIMELINE ||
      region->regiontype != RGN_TYPE_WINDOW)
  {
    return false;
  }
  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(
      area->spacedata.first);
  if (!better_timeline_is_in_track_list_pane(region, sbetter_timeline, event->mval[0])) {
    return false;
  }
  const int track_idx = better_timeline_track_from_region_y(
      region, sbetter_timeline, event->mval[1]);
  if (track_idx < 0) {
    return false;
  }
  const BetterTimelineTrack *track = better_timeline_track_at_index(sbetter_timeline, track_idx);
  if (track == nullptr) {
    return false;
  }
  const ed::better_timeline::BetterTimelineTrackType *tt =
      ed::better_timeline::track_type_find_from_idname(track->track_type);
  return (tt != nullptr && tt->has_object_slot);
}

static void better_timeline_track_drop_object_copy(bContext * /*C*/,
                                                   wmDrag *drag,
                                                   wmDropBox *drop)
{
  ID *id = WM_drag_get_local_ID(drag, ID_OB);
  if (id != nullptr) {
    RNA_int_set(drop->ptr, "session_uid", int(id->session_uid));
  }
}

static wmOperatorStatus better_timeline_track_drop_object_invoke(bContext *C,
                                                                  wmOperator *op,
                                                                  const wmEvent *event)
{
  const ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(
      CTX_wm_area(C)->spacedata.first);

  const int track_idx = better_timeline_track_from_region_y(
      region, sbetter_timeline, event->mval[1]);
  BetterTimelineTrack *track = better_timeline_track_at_index(sbetter_timeline, track_idx);
  if (track == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const ed::better_timeline::BetterTimelineTrackType *tt =
      ed::better_timeline::track_type_find_from_idname(track->track_type);
  if (tt == nullptr || !tt->has_object_slot) {
    return OPERATOR_CANCELLED;
  }

  Main *bmain = CTX_data_main(C);
  const uint32_t session_uid = uint32_t(RNA_int_get(op->ptr, "session_uid"));
  Object *ob = reinterpret_cast<Object *>(
      BKE_libblock_find_session_uid(bmain, ID_OB, session_uid));
  if (ob == nullptr) {
    return OPERATOR_CANCELLED;
  }

  better_timeline_undo_push_init(C, "Assign Track Object");
  track->object = ob;
  better_timeline_tag_space_state_changed(C);

  return OPERATOR_FINISHED;
}

/* -------------------------------------------------------------------- */
/** \name Track Pick Object Operator
 * \{ */

static int object_type_to_icon(const short ob_type)
{
  switch (ob_type) {
    case OB_MESH:
      return ICON_OUTLINER_OB_MESH;
    case OB_CAMERA:
      return ICON_OUTLINER_OB_CAMERA;
    case OB_LAMP:
      return ICON_OUTLINER_OB_LIGHT;
    case OB_ARMATURE:
      return ICON_OUTLINER_OB_ARMATURE;
    case OB_CURVES:
      return ICON_OUTLINER_OB_CURVES;
    case OB_EMPTY:
      return ICON_OUTLINER_OB_EMPTY;
    case OB_LATTICE:
      return ICON_OUTLINER_OB_LATTICE;
    case OB_SPEAKER:
      return ICON_OUTLINER_OB_SPEAKER;
    default:
      return ICON_OBJECT_DATA;
  }
}

static const EnumPropertyItem *better_timeline_pick_object_enum_items_fn(
    bContext *C, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free)
{
  if (C == nullptr) {
    *r_free = false;
    return rna_enum_dummy_NULL_items;
  }

  Main *bmain = CTX_data_main(C);
  EnumPropertyItem *items = nullptr;
  int totitem = 0;

  /* "None" — clears the binding. */
  {
    EnumPropertyItem item = {};
    item.value = 0;
    item.identifier = "NONE";
    item.name = "None";
    item.icon = ICON_X;
    RNA_enum_item_add(&items, &totitem, &item);
  }

  for (Object *ob = static_cast<Object *>(bmain->objects.first); ob != nullptr;
       ob = static_cast<Object *>(ob->id.next))
  {
    EnumPropertyItem item = {};
    item.value = int(ob->id.session_uid);
    item.identifier = ob->id.name; /* type-prefixed — unique across all IDs */
    item.name = ob->id.name + 2;
    item.icon = object_type_to_icon(ob->type);
    RNA_enum_item_add(&items, &totitem, &item);
  }

  RNA_enum_item_end(&items, &totitem);
  *r_free = true;
  return items;
}

static wmOperatorStatus better_timeline_track_pick_object_exec(bContext *C, wmOperator *op)
{
  ScrArea *area = CTX_wm_area(C);
  if (area == nullptr) {
    return OPERATOR_CANCELLED;
  }
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);
  const int track_index = RNA_int_get(op->ptr, "track_index");
  BetterTimelineTrack *track = better_timeline_track_at_index(sbetter_timeline, track_index);
  if (track == nullptr) {
    return OPERATOR_CANCELLED;
  }

  Main *bmain = CTX_data_main(C);
  const int uid = RNA_enum_get(op->ptr, "object");
  Object *ob = nullptr;
  if (uid != 0) {
    ob = reinterpret_cast<Object *>(
        BKE_libblock_find_session_uid(bmain, ID_OB, uint32_t(uid)));
  }

  better_timeline_undo_push_init(C, "Pick Track Object");
  track->object = ob;
  better_timeline_tag_space_state_changed(C);
  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_track_pick_object(wmOperatorType *ot)
{
  ot->name = "Pick Track Object";
  ot->idname = "BETTER_TIMELINE_OT_track_pick_object";
  ot->description = "Select the scene object to bind to this track";

  ot->exec = better_timeline_track_pick_object_exec;
  ot->invoke = WM_enum_search_invoke;
  ot->poll = better_timeline_operator_region_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "object", rna_enum_dummy_NULL_items, 0, "Object", "Scene object to bind");
  RNA_def_enum_funcs(prop, better_timeline_pick_object_enum_items_fn);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
  ot->prop = prop;

  RNA_def_int(ot->srna, "track_index", -1, -1, INT_MAX, "Track Index", "", -1, INT_MAX);
}

/** \} */

static void BETTER_TIMELINE_OT_track_drop_object(wmOperatorType *ot)
{
  ot->name = "Drop Object onto Track";
  ot->idname = "BETTER_TIMELINE_OT_track_drop_object";
  ot->description = "Assign a scene object to the track by dropping it from the Outliner";

  ot->invoke = better_timeline_track_drop_object_invoke;
  ot->poll = better_timeline_operator_region_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "session_uid",
              0,
              0,
              INT_MAX,
              "Session UID",
              "Session UID of the object to assign",
              0,
              INT_MAX);
}

/** \} */

void better_timeline_drop_register()
{
  /* Called by WM as st->dropboxes — registers the drop handler into the map. */
  ListBaseT<wmDropBox> *lb = WM_dropboxmap_find(
      BETTER_TIMELINE_KEYMAP_NAME, SPACE_BETTER_TIMELINE, RGN_TYPE_WINDOW);
  wmDropBox *drop = WM_dropbox_add(lb,
                                   "BETTER_TIMELINE_OT_track_drop_object",
                                   better_timeline_track_drop_object_poll,
                                   better_timeline_track_drop_object_copy,
                                   nullptr,
                                   nullptr);
  drop->draw_droptip = WM_drag_draw_item_name_fn;
}

}  // namespace blender
