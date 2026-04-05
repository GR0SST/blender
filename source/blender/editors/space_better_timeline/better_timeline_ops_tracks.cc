/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#include <algorithm>

#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"

#include "ED_better_timeline.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

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

static void better_timeline_track_clipboard_clear()
{
  better_timeline_tracks_free(&g_better_timeline_track_clipboard);
}

static bool better_timeline_track_select_poll(bContext *C)
{
  return better_timeline_operator_region_poll(C);
}

static wmOperatorStatus better_timeline_track_select_invoke(bContext *C,
                                                            wmOperator * /*op*/,
                                                            const wmEvent *event)
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

  better_timeline_track_drag_visual_state_clear();
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
          region, reorder_data->dragged_track, reorder_data->current_insertion_index);
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
            region, reorder_data->dragged_track, reorder_data->current_insertion_index);
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
      region, dragged_track, reorder_data->current_insertion_index);
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
    if (track == nullptr) {
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
    if (track == nullptr) {
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

void better_timeline_track_ops_register()
{
  WM_operatortype_append(BETTER_TIMELINE_OT_add_track);
  WM_operatortype_append(BETTER_TIMELINE_OT_add_track_menu);
  WM_operatortype_append(BETTER_TIMELINE_OT_delete_track);
  WM_operatortype_append(BETTER_TIMELINE_OT_clear_selection);
  WM_operatortype_append(BETTER_TIMELINE_OT_track_select);
  WM_operatortype_append(BETTER_TIMELINE_OT_track_reorder);
}

void better_timeline_clipboard_track_ops_register()
{
  WM_operatortype_append(BETTER_TIMELINE_OT_duplicate_track);
  WM_operatortype_append(BETTER_TIMELINE_OT_copy_track);
  WM_operatortype_append(BETTER_TIMELINE_OT_paste_track);
}

}  // namespace blender
