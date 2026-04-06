/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#include <algorithm>
#include <cmath>

#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_string.h"

#include "BKE_context.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_keymap.hh"
#include "WM_types.hh"

#include "better_timeline_intern.hh" /* own include */

namespace blender {

static bool better_timeline_time_scrub_event_in_region_poll(const wmWindow * /*win*/,
                                                            const ScrArea *area,
                                                            const ARegion *region,
                                                            const wmEvent *event)
{
  return better_timeline_scrub_event_in_region(area, region, event);
}

static bool better_timeline_scroll_tracks_poll(bContext *C)
{
  return better_timeline_operator_region_poll(C);
}

static wmOperatorStatus better_timeline_scroll_tracks_invoke(bContext *C,
                                                             wmOperator * /*op*/,
                                                             const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  const bool in_track_list_pane = better_timeline_is_in_track_list_pane(
      region, sbetter_timeline, event->mval[0]);
  const bool in_track_scrollbar = better_timeline_is_in_track_scrollbar(
      region, sbetter_timeline, event->mval[0], event->mval[1]);

  if (!in_track_list_pane && !in_track_scrollbar) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }
  if (better_timeline_scrub_event_in_region(area, region, event) ||
      better_timeline_is_on_panel_divider(region, sbetter_timeline, event->mval[0]))
  {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  const int scroll_max = better_timeline_track_scroll_max(region, sbetter_timeline);
  if (scroll_max == 0) {
    return OPERATOR_FINISHED;
  }

  const eBetterTimelineTrackScrollDirection direction =
      (event->type == WHEELUPMOUSE) ? BETTER_TIMELINE_TRACK_SCROLL_UP :
                                      BETTER_TIMELINE_TRACK_SCROLL_DOWN;
  const int scroll_step = BETTER_TIMELINE_ROW_HEIGHT * int(direction);
  const int new_scroll_offset = std::clamp(
      better_timeline_track_scroll_offset(region, sbetter_timeline) + scroll_step, 0, scroll_max);

  if (new_scroll_offset == sbetter_timeline->track_scroll_offset) {
    return OPERATOR_FINISHED;
  }

  sbetter_timeline->track_scroll_offset = new_scroll_offset;
  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_scroll_tracks(wmOperatorType *ot)
{
  ot->name = "Scroll Better Timeline Tracks";
  ot->idname = "BETTER_TIMELINE_OT_scroll_tracks";
  ot->description = "Scroll the track list in Better Timeline";

  ot->invoke = better_timeline_scroll_tracks_invoke;
  ot->poll = better_timeline_scroll_tracks_poll;

  ot->flag = OPTYPE_INTERNAL;
}

static bool better_timeline_scrollbar_drag_poll(bContext *C)
{
  return better_timeline_operator_region_poll(C);
}

static wmOperatorStatus better_timeline_scrollbar_drag_modal(bContext *C,
                                                             wmOperator *op,
                                                             const wmEvent *event)
{
  auto *drag_data = static_cast<BetterTimelineScrollbarDragData *>(op->customdata);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  switch (event->type) {
    case MOUSEMOVE: {
      const rcti scrollbar_rect = better_timeline_track_scrollbar_rect(region, sbetter_timeline);
      const rcti thumb_rect = better_timeline_track_scrollbar_thumb_rect(region, sbetter_timeline);
      const int scroll_max = better_timeline_track_scroll_max(region, sbetter_timeline);
      const int travel = std::max(
          1, BLI_rcti_size_y(&scrollbar_rect) - BLI_rcti_size_y(&thumb_rect));
      const int mouse_delta = drag_data->initial_mouse_y - event->mval[1];
      const int scroll_delta = int(
          std::round(float(mouse_delta) * float(scroll_max) / float(travel)));
      sbetter_timeline->track_scroll_offset = std::clamp(
          drag_data->initial_scroll_offset + scroll_delta, 0, scroll_max);
      ED_area_tag_redraw(area);
      break;
    }
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        MEM_delete(drag_data);
        op->customdata = nullptr;
        return OPERATOR_FINISHED;
      }
      break;
    case EVT_ESCKEY:
      sbetter_timeline->track_scroll_offset = drag_data->initial_scroll_offset;
      ED_area_tag_redraw(area);
      MEM_delete(drag_data);
      op->customdata = nullptr;
      return OPERATOR_CANCELLED;
    default:
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus better_timeline_scrollbar_drag_invoke(bContext *C,
                                                              wmOperator *op,
                                                              const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  if (!better_timeline_is_in_track_scrollbar(
          region, sbetter_timeline, event->mval[0], event->mval[1]))
  {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  const int scroll_max = better_timeline_track_scroll_max(region, sbetter_timeline);
  if (scroll_max == 0) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  auto *drag_data = MEM_new<BetterTimelineScrollbarDragData>(__func__);
  drag_data->initial_mouse_y = event->mval[1];
  drag_data->initial_scroll_offset = better_timeline_track_scroll_offset(region, sbetter_timeline);
  op->customdata = drag_data;

  const rcti thumb_rect = better_timeline_track_scrollbar_thumb_rect(region, sbetter_timeline);
  if (!BLI_rcti_isect_pt(&thumb_rect, event->mval[0], event->mval[1])) {
    const rcti scrollbar_rect = better_timeline_track_scrollbar_rect(region, sbetter_timeline);
    const int travel = std::max(
        1, BLI_rcti_size_y(&scrollbar_rect) - BLI_rcti_size_y(&thumb_rect));
    const int click_top_offset = std::clamp(
        scrollbar_rect.ymax - event->mval[1] - (BLI_rcti_size_y(&thumb_rect) / 2), 0, travel);
    sbetter_timeline->track_scroll_offset = int(
        std::round(float(click_top_offset) * float(scroll_max) / float(travel)));
    drag_data->initial_scroll_offset = sbetter_timeline->track_scroll_offset;
    drag_data->initial_mouse_y = event->mval[1];
    ED_area_tag_redraw(area);
  }

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static void BETTER_TIMELINE_OT_scrollbar_drag(wmOperatorType *ot)
{
  ot->name = "Drag Better Timeline Scrollbar";
  ot->idname = "BETTER_TIMELINE_OT_scrollbar_drag";
  ot->description = "Drag the Better Timeline track scrollbar";

  ot->invoke = better_timeline_scrollbar_drag_invoke;
  ot->modal = better_timeline_scrollbar_drag_modal;
  ot->poll = better_timeline_scrollbar_drag_poll;

  ot->flag = OPTYPE_BLOCKING;
}

static bool better_timeline_resize_panel_poll(bContext *C)
{
  return better_timeline_operator_region_poll(C);
}

static wmOperatorStatus better_timeline_resize_panel_modal(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent *event)
{
  auto *resize_data = static_cast<BetterTimelinePanelResizeData *>(op->customdata);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  switch (event->type) {
    case MOUSEMOVE: {
      const int width = resize_data->initial_panel_width +
                        (event->mval[0] - resize_data->initial_mouse_x);
      sbetter_timeline->track_panel_width = better_timeline_panel_width_clamp(region, width);
      better_timeline_view2d_update_old_window(region, sbetter_timeline);
      ED_area_tag_redraw(area);
      break;
    }
    case LEFTMOUSE:
    case RIGHTMOUSE:
    case MIDDLEMOUSE:
      if (event->val == KM_RELEASE) {
        WM_cursor_modal_restore(CTX_wm_window(C));
        MEM_delete(resize_data);
        op->customdata = nullptr;
        return OPERATOR_FINISHED;
      }
      break;
    case EVT_ESCKEY:
      sbetter_timeline->track_panel_width = resize_data->initial_panel_width;
      better_timeline_view2d_update_old_window(region, sbetter_timeline);
      ED_area_tag_redraw(area);
      WM_cursor_modal_restore(CTX_wm_window(C));
      MEM_delete(resize_data);
      op->customdata = nullptr;
      return OPERATOR_CANCELLED;
    default:
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus better_timeline_resize_panel_invoke(bContext *C,
                                                            wmOperator *op,
                                                            const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  if (!better_timeline_is_on_panel_divider(region, sbetter_timeline, event->mval[0])) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  auto *resize_data = MEM_new<BetterTimelinePanelResizeData>(__func__);
  resize_data->initial_mouse_x = event->mval[0];
  resize_data->initial_panel_width = better_timeline_left_panel_width(region, sbetter_timeline);
  op->customdata = resize_data;

  better_timeline_view2d_update_old_window(region, sbetter_timeline);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_X_MOVE);
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static void BETTER_TIMELINE_OT_resize_panel(wmOperatorType *ot)
{
  ot->name = "Resize Better Timeline Track Panel";
  ot->idname = "BETTER_TIMELINE_OT_resize_panel";
  ot->description = "Resize the track list panel in Better Timeline";

  ot->invoke = better_timeline_resize_panel_invoke;
  ot->modal = better_timeline_resize_panel_modal;
  ot->poll = better_timeline_resize_panel_poll;

  ot->flag = OPTYPE_BLOCKING;
}

static bool better_timeline_view_all_poll(bContext *C)
{
  return better_timeline_operator_region_poll(C);
}

static wmOperatorStatus better_timeline_view_all_exec(bContext *C, wmOperator * /*op*/)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  Scene *scene = CTX_data_scene(C);
  View2D *v2d = &region->v2d;
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  if (scene == nullptr) {
    return OPERATOR_CANCELLED;
  }

  better_timeline_view_sync(region, scene, sbetter_timeline);

  const float frame_start = float(scene->r.sfra);
  const float frame_end = std::max(frame_start + 1.0f, float(scene->r.efra) + 1.0f);

  v2d->cur.xmin = frame_start;
  v2d->cur.xmax = frame_end;
  v2d->cur.ymin = v2d->tot.ymin;
  v2d->cur.ymax = v2d->tot.ymax;

  ui::view2d_curRect_changed(C, v2d);
  ED_region_tag_redraw(region);
  ui::view2d_sync(CTX_wm_screen(C), area, v2d, V2D_LOCK_COPY);

  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_view_all(wmOperatorType *ot)
{
  ot->name = "Frame Better Timeline";
  ot->idname = "BETTER_TIMELINE_OT_view_all";
  ot->description = "Show the full scene frame range in Better Timeline";

  ot->exec = better_timeline_view_all_exec;
  ot->poll = better_timeline_view_all_poll;
}

static void better_timeline_keymap_ensure(wmWindowManager *wm)
{
  wmKeyMap *keymap = WM_keymap_ensure(
      wm->runtime->defaultconf, BETTER_TIMELINE_KEYMAP_NAME, SPACE_BETTER_TIMELINE, RGN_TYPE_WINDOW);

  bool has_track_select = false;
  bool has_track_select_shift = false;
  bool has_track_select_oskey = false;
  bool has_clip_drag = false;
  bool has_clip_drag_shift = false;
  bool has_clip_drag_oskey = false;
  bool has_clip_move = false;
  bool has_delete_clip_del = false;
  bool has_delete_clip_x = false;
  bool has_add_track = false;
  bool has_add_track_menu = false;
  bool has_delete_track_del = false;
  bool has_delete_track_x = false;
  bool has_clear_selection = false;
  bool has_scrollbar_drag = false;
  bool has_scroll_tracks_up = false;
  bool has_scroll_tracks_down = false;
  bool has_track_reorder = false;
  bool has_panel_resize = false;
  bool has_pan = false;
  bool has_zoom_in = false;
  bool has_zoom_out = false;
  bool has_view_all = false;
  bool has_play_toggle = false;
  bool has_properties_toggle = false;
  bool has_undo = false;
  bool has_redo = false;
  bool has_duplicate_clip = false;
  bool has_duplicate_track = false;
  bool has_copy_clip = false;
  bool has_copy_track = false;
  bool has_paste_clip = false;
  bool has_paste_track = false;

  wmKeyMapItem *kmi_next = nullptr;
  for (wmKeyMapItem *kmi = static_cast<wmKeyMapItem *>(keymap->items.first); kmi != nullptr;
       kmi = kmi_next)
  {
    kmi_next = kmi->next;
    if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_add_track")) {
      if (kmi->type == LEFTMOUSE && kmi->val == KM_PRESS) {
        kmi->val = KM_RELEASE;
        WM_keyconfig_update_tag(keymap, kmi);
      }
      has_add_track = (kmi->type == LEFTMOUSE && kmi->val == KM_RELEASE);
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_clip_drag")) {
      if (kmi->type == LEFTMOUSE && kmi->val == KM_PRESS_DRAG) {
        kmi->val = KM_PRESS;
        WM_keyconfig_update_tag(keymap, kmi);
      }
      if (kmi->type == LEFTMOUSE && kmi->val == KM_PRESS) {
        if (kmi->shift == KM_MOD_HELD) {
          has_clip_drag_shift = true;
        }
        else if (kmi->oskey == KM_MOD_HELD) {
          has_clip_drag_oskey = true;
        }
        else if (kmi->shift == KM_NOTHING && kmi->oskey == KM_NOTHING) {
          has_clip_drag = true;
        }
      }
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_clip_select")) {
      WM_keymap_remove_item(keymap, kmi);
      continue;
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_move_clip")) {
      has_clip_move = (kmi->type == EVT_GKEY && kmi->val == KM_PRESS);
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_delete_clip")) {
      if (kmi->type == EVT_DELKEY) {
        has_delete_clip_del = true;
      }
      else if (kmi->type == EVT_XKEY) {
        has_delete_clip_x = true;
      }
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_add_track_menu")) {
      has_add_track_menu = true;
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_delete_track")) {
      if (kmi->type == EVT_DELKEY) {
        has_delete_track_del = true;
      }
      else if (kmi->type == EVT_XKEY) {
        has_delete_track_x = true;
      }
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_clear_selection")) {
      has_clear_selection = true;
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_scrollbar_drag")) {
      has_scrollbar_drag = true;
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_scroll_tracks")) {
      if (kmi->type == WHEELUPMOUSE) {
        has_scroll_tracks_up = true;
      }
      else if (kmi->type == WHEELDOWNMOUSE) {
        has_scroll_tracks_down = true;
      }
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_track_select")) {
      if (kmi->type == LEFTMOUSE) {
        if (kmi->shift == KM_MOD_HELD) {
          has_track_select_shift = true;
        }
        else if (kmi->oskey == KM_MOD_HELD) {
          has_track_select_oskey = true;
        }
        else if (kmi->shift == KM_NOTHING && kmi->oskey == KM_NOTHING) {
          has_track_select = true;
        }
      }
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_track_reorder")) {
      has_track_reorder = (kmi->type == LEFTMOUSE && kmi->val == KM_PRESS_DRAG);
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_resize_panel")) {
      has_panel_resize = true;
    }
    else if (STREQ(kmi->idname, "VIEW2D_OT_pan")) {
      has_pan = true;
    }
    else if (STREQ(kmi->idname, "VIEW2D_OT_zoom_in")) {
      has_zoom_in = true;
    }
    else if (STREQ(kmi->idname, "VIEW2D_OT_zoom_out")) {
      has_zoom_out = true;
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_view_all")) {
      has_view_all = true;
    }
    else if (STREQ(kmi->idname, "SCREEN_OT_animation_play")) {
      has_play_toggle = true;
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_toggle_properties_panel")) {
      has_properties_toggle = (kmi->type == EVT_NKEY && kmi->val == KM_PRESS);
    }
    else if (STREQ(kmi->idname, "ED_OT_undo")) {
      has_undo = (kmi->type == EVT_ZKEY && kmi->val == KM_PRESS && kmi->oskey == KM_MOD_HELD &&
                  kmi->shift == KM_NOTHING);
    }
    else if (STREQ(kmi->idname, "ED_OT_redo")) {
      has_redo = (kmi->type == EVT_ZKEY && kmi->val == KM_PRESS && kmi->oskey == KM_MOD_HELD &&
                  kmi->shift == KM_MOD_HELD);
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_duplicate_clip")) {
      has_duplicate_clip = (kmi->type == EVT_DKEY && kmi->val == KM_PRESS &&
                            kmi->shift == KM_MOD_HELD);
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_duplicate_track")) {
      has_duplicate_track = (kmi->type == EVT_DKEY && kmi->val == KM_PRESS &&
                             kmi->shift == KM_MOD_HELD);
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_copy_clip")) {
      has_copy_clip = (kmi->type == EVT_CKEY && kmi->val == KM_PRESS && kmi->oskey == KM_MOD_HELD);
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_copy_track")) {
      has_copy_track = (kmi->type == EVT_CKEY && kmi->val == KM_PRESS &&
                        kmi->oskey == KM_MOD_HELD);
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_paste_clip")) {
      has_paste_clip = (kmi->type == EVT_VKEY && kmi->val == KM_PRESS &&
                        kmi->oskey == KM_MOD_HELD);
    }
    else if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_paste_track")) {
      has_paste_track = (kmi->type == EVT_VKEY && kmi->val == KM_PRESS &&
                         kmi->oskey == KM_MOD_HELD);
    }
  }

  if (!has_panel_resize) {
    KeyMapItem_Params params{};
    params.type = LEFTMOUSE;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_resize_panel", &params);
  }

  if (!has_add_track) {
    KeyMapItem_Params params{};
    params.type = LEFTMOUSE;
    params.value = KM_RELEASE;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_add_track", &params);
  }

  if (!has_clip_drag) {
    KeyMapItem_Params params{};
    params.type = LEFTMOUSE;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_clip_drag", &params);
  }

  if (!has_clip_drag_shift) {
    KeyMapItem_Params params{};
    params.type = LEFTMOUSE;
    params.value = KM_PRESS;
    params.modifier = KM_SHIFT;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_clip_drag", &params);
  }

  if (!has_clip_drag_oskey) {
    KeyMapItem_Params params{};
    params.type = LEFTMOUSE;
    params.value = KM_PRESS;
    params.modifier = KM_OSKEY;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_clip_drag", &params);
  }

  if (!has_clip_move) {
    KeyMapItem_Params params{};
    params.type = EVT_GKEY;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_move_clip", &params);
  }

  if (!has_delete_clip_del) {
    KeyMapItem_Params params{};
    params.type = EVT_DELKEY;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_delete_clip", &params);
  }

  if (!has_delete_clip_x) {
    KeyMapItem_Params params{};
    params.type = EVT_XKEY;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_delete_clip", &params);
  }

  if (!has_add_track_menu) {
    KeyMapItem_Params params{};
    params.type = EVT_AKEY;
    params.value = KM_PRESS;
    params.modifier = KM_SHIFT;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_add_track_menu", &params);
  }

  if (!has_scrollbar_drag) {
    KeyMapItem_Params params{};
    params.type = LEFTMOUSE;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_scrollbar_drag", &params);
  }

  if (!has_track_select) {
    KeyMapItem_Params params{};
    params.type = LEFTMOUSE;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_track_select", &params);
  }

  if (!has_track_select_shift) {
    KeyMapItem_Params params{};
    params.type = LEFTMOUSE;
    params.value = KM_PRESS;
    params.modifier = KM_SHIFT;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_track_select", &params);
  }

  if (!has_track_select_oskey) {
    KeyMapItem_Params params{};
    params.type = LEFTMOUSE;
    params.value = KM_PRESS;
    params.modifier = KM_OSKEY;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_track_select", &params);
  }

  if (!has_track_reorder) {
    KeyMapItem_Params params{};
    params.type = LEFTMOUSE;
    params.value = KM_PRESS_DRAG;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_track_reorder", &params);
  }

  if (!has_delete_track_del) {
    KeyMapItem_Params params{};
    params.type = EVT_DELKEY;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_delete_track", &params);
  }

  if (!has_delete_track_x) {
    KeyMapItem_Params params{};
    params.type = EVT_XKEY;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_delete_track", &params);
  }

  if (!has_clear_selection) {
    KeyMapItem_Params params{};
    params.type = EVT_ESCKEY;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_clear_selection", &params);
  }

  if (!has_undo) {
    KeyMapItem_Params params{};
    params.type = EVT_ZKEY;
    params.value = KM_PRESS;
    params.modifier = KM_OSKEY;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "ED_OT_undo", &params);
  }

  if (!has_redo) {
    KeyMapItem_Params params{};
    params.type = EVT_ZKEY;
    params.value = KM_PRESS;
    params.modifier = KM_OSKEY | KM_SHIFT;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "ED_OT_redo", &params);
  }

  if (!has_duplicate_clip) {
    KeyMapItem_Params params{};
    params.type = EVT_DKEY;
    params.value = KM_PRESS;
    params.modifier = KM_SHIFT;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_duplicate_clip", &params);
  }

  if (!has_duplicate_track) {
    KeyMapItem_Params params{};
    params.type = EVT_DKEY;
    params.value = KM_PRESS;
    params.modifier = KM_SHIFT;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_duplicate_track", &params);
  }

  if (!has_copy_clip) {
    KeyMapItem_Params params{};
    params.type = EVT_CKEY;
    params.value = KM_PRESS;
    params.modifier = KM_OSKEY;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_copy_clip", &params);
  }

  if (!has_copy_track) {
    KeyMapItem_Params params{};
    params.type = EVT_CKEY;
    params.value = KM_PRESS;
    params.modifier = KM_OSKEY;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_copy_track", &params);
  }

  if (!has_paste_clip) {
    KeyMapItem_Params params{};
    params.type = EVT_VKEY;
    params.value = KM_PRESS;
    params.modifier = KM_OSKEY;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_paste_clip", &params);
  }

  if (!has_paste_track) {
    KeyMapItem_Params params{};
    params.type = EVT_VKEY;
    params.value = KM_PRESS;
    params.modifier = KM_OSKEY;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_paste_track", &params);
  }

  if (!has_scroll_tracks_up) {
    KeyMapItem_Params params{};
    params.type = WHEELUPMOUSE;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_scroll_tracks", &params);
  }

  if (!has_scroll_tracks_down) {
    KeyMapItem_Params params{};
    params.type = WHEELDOWNMOUSE;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_scroll_tracks", &params);
  }

  if (!has_pan) {
    KeyMapItem_Params params{};
    params.type = MIDDLEMOUSE;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "VIEW2D_OT_pan", &params);
  }

  if (!has_zoom_in) {
    KeyMapItem_Params params{};
    params.type = WHEELUPMOUSE;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "VIEW2D_OT_zoom_in", &params);
  }

  if (!has_zoom_out) {
    KeyMapItem_Params params{};
    params.type = WHEELDOWNMOUSE;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "VIEW2D_OT_zoom_out", &params);
  }

  if (!has_view_all) {
    KeyMapItem_Params params{};
    params.type = EVT_HOMEKEY;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_view_all", &params);
  }

  if (!has_play_toggle) {
    KeyMapItem_Params params{};
    params.type = EVT_SPACEKEY;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "SCREEN_OT_animation_play", &params);
  }

  if (!has_properties_toggle) {
    KeyMapItem_Params params{};
    params.type = EVT_NKEY;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_toggle_properties_panel", &params);
  }
}

static bool better_timeline_toggle_properties_panel_poll(bContext *C)
{
  return better_timeline_operator_region_poll(C);
}

static wmOperatorStatus better_timeline_toggle_properties_panel_exec(bContext *C, wmOperator * /*op*/)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *ui_region = BKE_area_find_region_type(area, RGN_TYPE_UI);
  if (ui_region == nullptr) {
    return OPERATOR_CANCELLED;
  }

  ED_region_toggle_hidden(C, ui_region);
  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_toggle_properties_panel(wmOperatorType *ot)
{
  ot->name = "Toggle Better Timeline Properties";
  ot->idname = "BETTER_TIMELINE_OT_toggle_properties_panel";
  ot->description = "Show or hide the Better Timeline properties sidebar";

  ot->exec = better_timeline_toggle_properties_panel_exec;
  ot->poll = better_timeline_toggle_properties_panel_poll;

  ot->flag = OPTYPE_INTERNAL;
}

void better_timeline_main_region_keymap_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;

  better_timeline_keymap_ensure(wm);

  keymap = WM_keymap_ensure(
      wm->runtime->defaultconf, BETTER_TIMELINE_KEYMAP_NAME, SPACE_BETTER_TIMELINE, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);

  keymap = WM_keymap_ensure(wm->runtime->defaultconf, "View2D", SPACE_EMPTY, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);

  keymap = WM_keymap_ensure(wm->runtime->defaultconf, "Time Scrub", SPACE_EMPTY, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler_poll(
      &region->runtime->handlers, keymap, better_timeline_time_scrub_event_in_region_poll);

  keymap = WM_keymap_ensure(
      wm->runtime->defaultconf, "View2D Buttons List", SPACE_EMPTY, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);
}

void better_timeline_view_ops_register()
{
  WM_operatortype_append(BETTER_TIMELINE_OT_scrollbar_drag);
  WM_operatortype_append(BETTER_TIMELINE_OT_scroll_tracks);
  WM_operatortype_append(BETTER_TIMELINE_OT_resize_panel);
  WM_operatortype_append(BETTER_TIMELINE_OT_view_all);
  WM_operatortype_append(BETTER_TIMELINE_OT_toggle_properties_panel);
}

}  // namespace blender
