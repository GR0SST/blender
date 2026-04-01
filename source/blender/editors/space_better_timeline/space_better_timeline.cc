/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#include <algorithm>
#include <cstring>

#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"

#include "ED_anim_api.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_time_scrub_ui.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "BLF_api.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "BLO_read_write.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender {

static constexpr int BETTER_TIMELINE_MIN_ROWS = 6;
static constexpr int BETTER_TIMELINE_ROW_HEIGHT = 34;
static constexpr int BETTER_TIMELINE_TEST_TRACK_COUNT = 2;
static constexpr int BETTER_TIMELINE_PANEL_DEFAULT_WIDTH = 270;
static constexpr int BETTER_TIMELINE_PANEL_MIN_WIDTH = 180;
static constexpr int BETTER_TIMELINE_TIMELINE_MIN_WIDTH = 120;
static constexpr int BETTER_TIMELINE_DIVIDER_HIT_WIDTH = 5;
static constexpr const char *BETTER_TIMELINE_KEYMAP_NAME = "Better Timeline";

struct BetterTimelinePanelResizeData {
  int initial_mouse_x;
  int initial_panel_width;
};

static void better_timeline_view_sync(ARegion *region,
                                      const Scene *scene,
                                      const SpaceBetterTimeline *sbetter_timeline);

static int better_timeline_panel_width_clamp(const ARegion *region, const int panel_width)
{
  const int hard_max_width = std::max(1, region->winx - 1);
  const int min_width = std::min(int(BETTER_TIMELINE_PANEL_MIN_WIDTH * UI_SCALE_FAC),
                                 hard_max_width);
  const int max_width = std::clamp(region->winx - int(BETTER_TIMELINE_TIMELINE_MIN_WIDTH * UI_SCALE_FAC),
                                   min_width,
                                   hard_max_width);
  return std::clamp(panel_width, min_width, max_width);
}

static int better_timeline_default_left_panel_width(const ARegion *region)
{
  return better_timeline_panel_width_clamp(region, int(BETTER_TIMELINE_PANEL_DEFAULT_WIDTH * UI_SCALE_FAC));
}

static int better_timeline_left_panel_width(const ARegion *region,
                                            const SpaceBetterTimeline *sbetter_timeline)
{
  const int stored_width = (sbetter_timeline != nullptr) ? sbetter_timeline->track_panel_width : 0;
  const int panel_width = (stored_width > 0) ? stored_width : better_timeline_default_left_panel_width(region);
  return better_timeline_panel_width_clamp(region, panel_width);
}

static bool better_timeline_is_on_panel_divider(const ARegion *region,
                                                const SpaceBetterTimeline *sbetter_timeline,
                                                const int region_x)
{
  const int divider_x = better_timeline_left_panel_width(region, sbetter_timeline);
  return abs(region_x - divider_x) <= BETTER_TIMELINE_DIVIDER_HIT_WIDTH;
}

static int better_timeline_content_height(const ARegion *region)
{
  return std::max(0, region->winy - int(UI_TIME_SCRUB_MARGIN_Y));
}

static int better_timeline_row_count(const ARegion *region)
{
  const int content_height = better_timeline_content_height(region);
  return std::max(BETTER_TIMELINE_MIN_ROWS, content_height / BETTER_TIMELINE_ROW_HEIGHT);
}

static int better_timeline_visible_track_count(const ARegion *region)
{
  return std::min(better_timeline_row_count(region), BETTER_TIMELINE_TEST_TRACK_COUNT);
}

static float better_timeline_row_ymax(const int content_top, const int row_index)
{
  return float(content_top - (row_index * BETTER_TIMELINE_ROW_HEIGHT));
}

static float better_timeline_row_ymin(const int content_top, const int row_index)
{
  return float(std::max(0, int(better_timeline_row_ymax(content_top, row_index)) -
                               BETTER_TIMELINE_ROW_HEIGHT));
}

static int better_timeline_track_from_region_y(const ARegion *region, const int region_y)
{
  const int content_top = better_timeline_content_height(region);
  const int visible_track_count = better_timeline_visible_track_count(region);

  if (region_y >= content_top) {
    return -1;
  }
  if (region_y < content_top - (visible_track_count * BETTER_TIMELINE_ROW_HEIGHT)) {
    return -1;
  }

  return (content_top - region_y - 1) / BETTER_TIMELINE_ROW_HEIGHT;
}

static bool better_timeline_track_select_poll(bContext *C)
{
  const ScrArea *area = CTX_wm_area(C);
  const ARegion *region = CTX_wm_region(C);
  return area != nullptr && region != nullptr && area->spacetype == SPACE_BETTER_TIMELINE &&
         region->regiontype == RGN_TYPE_WINDOW;
}

static wmOperatorStatus better_timeline_track_select_invoke(bContext *C,
                                                            wmOperator * /*op*/,
                                                            const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  if (ED_time_scrub_event_in_region(region, event)) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }
  if (better_timeline_is_on_panel_divider(region, sbetter_timeline, event->mval[0])) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  const int clicked_track_index = better_timeline_track_from_region_y(region, event->mval[1]);
  if (sbetter_timeline->selected_track_index == clicked_track_index) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  sbetter_timeline->selected_track_index = clicked_track_index;
  ED_area_tag_redraw(area);

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

static bool better_timeline_resize_panel_poll(bContext *C)
{
  return better_timeline_track_select_poll(C);
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
      const int width = resize_data->initial_panel_width + (event->mval[0] - resize_data->initial_mouse_x);
      sbetter_timeline->track_panel_width = better_timeline_panel_width_clamp(region, width);
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
  return better_timeline_track_select_poll(C);
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
  bool has_panel_resize = false;
  bool has_pan = false;
  bool has_zoom_in = false;
  bool has_zoom_out = false;
  bool has_view_all = false;

  for (wmKeyMapItem *kmi = static_cast<wmKeyMapItem *>(keymap->items.first); kmi != nullptr;
       kmi = kmi->next)
  {
    if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_track_select")) {
      has_track_select = true;
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
  }

  if (!has_panel_resize) {
    KeyMapItem_Params params{};
    params.type = LEFTMOUSE;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_resize_panel", &params);
  }

  if (!has_track_select) {
    KeyMapItem_Params params{};
    params.type = LEFTMOUSE;
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_track_select", &params);
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
}

static void better_timeline_view_sync(ARegion *region,
                                      const Scene *scene,
                                      const SpaceBetterTimeline *sbetter_timeline)
{
  View2D *v2d = &region->v2d;

  const float frame_start = float(scene->r.sfra);
  const float frame_end = float(scene->r.efra);
  const float frame_range = std::max(1.0f, frame_end - frame_start);
  const float frame_padding = std::max(18.0f, frame_range * 0.14f);
  const float total_rows = float(better_timeline_row_count(region));
  const bool has_valid_cur = BLI_rctf_size_x(&v2d->cur) > 0.0f;
  const float cur_xmin = v2d->cur.xmin;
  const float cur_xmax = v2d->cur.xmax;

  v2d->tot.xmin = frame_start - frame_padding;
  v2d->tot.xmax = frame_end + frame_padding;
  v2d->tot.ymin = 0.0f;
  v2d->tot.ymax = total_rows;

  v2d->min[0] = 1.0f;
  v2d->min[1] = 0.0f;
  v2d->max[0] = MAXFRAMEF;
  v2d->max[1] = total_rows;
  v2d->minzoom = 0.02f;
  v2d->maxzoom = 50.0f;
  v2d->keepzoom = V2D_LOCKZOOM_Y | V2D_LIMITZOOM | V2D_KEEPZOOM;
  v2d->keepofs = V2D_KEEPOFS_Y;
  v2d->align = V2D_ALIGN_NO_POS_Y;
  v2d->scroll = V2D_SCROLL_BOTTOM | V2D_SCROLL_HORIZONTAL_HANDLES;

  v2d->mask.xmin = better_timeline_left_panel_width(region, sbetter_timeline);
  v2d->mask.xmax = region->winx;
  v2d->mask.ymin = 0;
  v2d->mask.ymax = better_timeline_content_height(region);

  if (!has_valid_cur) {
    v2d->cur = v2d->tot;
  }
  else {
    v2d->cur.xmin = cur_xmin;
    v2d->cur.xmax = cur_xmax;
    v2d->cur.ymin = v2d->tot.ymin;
    v2d->cur.ymax = v2d->tot.ymax;
  }

  ui::view2d_curRect_validate(v2d);
}

static SpaceLink *better_timeline_create(const ScrArea * /*area*/, const Scene * /*scene*/)
{
  ARegion *region;
  SpaceBetterTimeline *sbetter_timeline;

  sbetter_timeline = MEM_new<SpaceBetterTimeline>("init better timeline");
  sbetter_timeline->spacetype = SPACE_BETTER_TIMELINE;
  sbetter_timeline->selected_track_index = -1;
  sbetter_timeline->track_panel_width = 0;

  region = BKE_area_region_new();
  BLI_addtail(&sbetter_timeline->regionbase, region);
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;

  region = BKE_area_region_new();
  BLI_addtail(&sbetter_timeline->regionbase, region);
  region->regiontype = RGN_TYPE_WINDOW;

  return reinterpret_cast<SpaceLink *>(sbetter_timeline);
}

static void better_timeline_free(SpaceLink * /*sl*/) {}

static void better_timeline_init(wmWindowManager * /*wm*/, ScrArea * /*area*/) {}

static SpaceLink *better_timeline_duplicate(SpaceLink *sl)
{
  SpaceBetterTimeline *sbetter_timeline = MEM_dupalloc(
      reinterpret_cast<SpaceBetterTimeline *>(sl));
  return reinterpret_cast<SpaceLink *>(sbetter_timeline);
}

static void better_timeline_main_region_init(wmWindowManager *wm, ARegion *region)
{
  wmKeyMap *keymap;

  ui::view2d_region_reinit(&region->v2d, ui::V2D_COMMONVIEW_CUSTOM, region->winx, region->winy);
  better_timeline_keymap_ensure(wm);

  keymap = WM_keymap_ensure(
      wm->runtime->defaultconf, BETTER_TIMELINE_KEYMAP_NAME, SPACE_BETTER_TIMELINE, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);

  keymap = WM_keymap_ensure(wm->runtime->defaultconf, "View2D", SPACE_EMPTY, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);

  keymap = WM_keymap_ensure(wm->runtime->defaultconf, "Time Scrub", SPACE_EMPTY, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler_poll(
      &region->runtime->handlers, keymap, ED_time_scrub_event_in_region_poll);

  keymap = WM_keymap_ensure(wm->runtime->defaultconf, "Animation", SPACE_EMPTY, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);

  keymap = WM_keymap_ensure(
      wm->runtime->defaultconf, "View2D Buttons List", SPACE_EMPTY, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);
}

static void better_timeline_main_region_cursor(wmWindow *win, ScrArea *area, ARegion *region)
{
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);
  const int region_x = win->runtime->eventstate->xy[0] - region->winrct.xmin;

  if (better_timeline_is_on_panel_divider(region, sbetter_timeline, region_x)) {
    WM_cursor_set(win, WM_CURSOR_X_MOVE);
    return;
  }

  WM_cursor_set(win, WM_CURSOR_DEFAULT);
}

static void better_timeline_draw_layout_overlay(const ARegion *region,
                                                const SpaceBetterTimeline *sbetter_timeline)
{
  const int left_panel_width = better_timeline_left_panel_width(region, sbetter_timeline);
  const int content_top = better_timeline_content_height(region);
  const int visible_track_count = better_timeline_visible_track_count(region);

  GPU_matrix_push_projection();
  wmOrtho2_region_pixelspace(region);

  GPU_blend(GPU_BLEND_ALPHA);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  immUniformColor4f(0.15f, 0.15f, 0.15f, 1.0f);
  immRectf(pos, 0.0f, 0.0f, float(left_panel_width), float(region->winy));

  immUniformColor4f(0.18f, 0.18f, 0.18f, 1.0f);
  immRectf(pos, 0.0f, float(content_top), float(left_panel_width), float(region->winy));

  immUniformColor4f(0.17f, 0.17f, 0.17f, 1.0f);
  immRectf(pos, 0.0f, 0.0f, float(left_panel_width), float(content_top));

  for (int row_index = 0; row_index < visible_track_count; row_index++) {
    const float y_max = better_timeline_row_ymax(content_top, row_index);
    const float y_min = better_timeline_row_ymin(content_top, row_index);

    if (row_index == sbetter_timeline->selected_track_index) {
      immUniformColor4f(0.25f, 0.40f, 0.72f, 0.92f);
      immRectf(pos, 0.0f, y_min, float(left_panel_width), y_max);

      immUniformColor4f(0.25f, 0.40f, 0.72f, 0.20f);
      immRectf(pos, float(left_panel_width), y_min, float(region->winx), y_max);
    }
    else {
      immUniformColor4f(1.0f, 1.0f, 1.0f, 0.055f);
      immRectf(pos, 0.0f, y_min, float(region->winx), y_max);
    }
  }

  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.08f);
  immBegin(GPU_PRIM_LINES, (visible_track_count + 2) * 2);
  for (int row_index = 0; row_index <= visible_track_count; row_index++) {
    const float y = float(content_top - (row_index * BETTER_TIMELINE_ROW_HEIGHT));
    immVertex2f(pos, 0.0f, y);
    immVertex2f(pos, float(region->winx), y);
  }
  immVertex2f(pos, float(left_panel_width), 0.0f);
  immVertex2f(pos, float(left_panel_width), float(region->winy));
  immEnd();

  immUniformColor4f(0.29f, 0.58f, 0.96f, 0.9f);
  immRectf(pos,
           float(left_panel_width),
           float(content_top - 2),
           float(region->winx),
           float(content_top));

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);

  constexpr const char *track_labels[BETTER_TIMELINE_TEST_TRACK_COUNT] = {"Track1", "Track2"};
  for (int row_index = 0; row_index < visible_track_count; row_index++) {
    uchar text_color[4];
    ui::theme::get_color_4ubv(
        row_index == sbetter_timeline->selected_track_index ? TH_HEADER_TEXT_HI : TH_TEXT,
        text_color);
    BLF_color4ubv(BLF_default(), text_color);
    const float y = better_timeline_row_ymin(content_top, row_index) +
                    (BETTER_TIMELINE_ROW_HEIGHT * 0.5f) - (5.0f * UI_SCALE_FAC);
    BLF_draw_default(
        16.0f * UI_SCALE_FAC, y, 0.0f, track_labels[row_index], BLF_DRAW_STR_DUMMY_MAX);
  }

  GPU_matrix_pop_projection();
}

static void better_timeline_main_region_draw(const bContext *C, ARegion *region)
{
  Scene *scene = CTX_data_scene(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);
  View2D *v2d = &region->v2d;

  if (scene == nullptr) {
    ui::theme::frame_buffer_clear(TH_BACK);
    return;
  }

  better_timeline_view_sync(region, scene, sbetter_timeline);

  ui::theme::frame_buffer_clear(TH_BACK);

  ui::view2d_view_ortho(v2d);
  ui::view2d_draw_lines_x_frames(v2d, scene, false, true, true);
  ANIM_draw_framerange(scene, v2d);
  ANIM_draw_cfra(C, v2d, DRAWCFRA_WIDE);
  ui::view2d_view_restore(C);

  ED_time_scrub_draw(region, scene, false, true, round_db_to_int(scene->frames_per_second()));
  better_timeline_draw_layout_overlay(region, sbetter_timeline);
}

static void better_timeline_main_region_draw_overlay(const bContext *C, ARegion *region)
{
  const Scene *scene = CTX_data_scene(C);
  if (scene == nullptr) {
    return;
  }

  /* Keep the scrub overlay in sync with the latest region dimensions during live resize. */
  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(CTX_wm_area(C)->spacedata.first);
  better_timeline_view_sync(region, scene, sbetter_timeline);
  ED_time_scrub_draw_current_frame(region, scene, false, false);
}

static void better_timeline_main_region_listener(const wmRegionListenerParams *params)
{
  ARegion *region = params->region;
  const wmNotifier *wmn = params->notifier;

  switch (wmn->category) {
    case NC_SCENE:
      if (ELEM(wmn->data, ND_FRAME, ND_FRAME_RANGE)) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SCREEN:
      if (wmn->data == ND_ANIMPLAY) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SPACE:
      ED_region_tag_redraw(region);
      break;
  }
}

static void better_timeline_header_region_init(wmWindowManager * /*wm*/, ARegion *region)
{
  ED_region_header_init(region);
}

static void better_timeline_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

static void better_timeline_header_region_listener(const wmRegionListenerParams *params)
{
  ARegion *region = params->region;
  const wmNotifier *wmn = params->notifier;

  switch (wmn->category) {
    case NC_SCREEN:
      if (ELEM(wmn->data, ND_LAYER, ND_ANIMPLAY)) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_WM:
      if (wmn->data == ND_JOB) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SPACE:
      ED_region_tag_redraw(region);
      break;
    case NC_ID:
      if (wmn->action == NA_RENAME) {
        ED_region_tag_redraw(region);
      }
      break;
  }
}

static void better_timeline_space_blend_write(BlendWriter *writer, SpaceLink *sl)
{
  writer->write_struct_cast<SpaceBetterTimeline>(sl);
}

void ED_spacetype_better_timeline()
{
  std::unique_ptr<SpaceType> st = std::make_unique<SpaceType>();
  ARegionType *art;

  WM_operatortype_append(BETTER_TIMELINE_OT_track_select);
  WM_operatortype_append(BETTER_TIMELINE_OT_resize_panel);
  WM_operatortype_append(BETTER_TIMELINE_OT_view_all);

  st->spaceid = SPACE_BETTER_TIMELINE;
  STRNCPY_UTF8(st->name, "Better Timeline");

  st->create = better_timeline_create;
  st->free = better_timeline_free;
  st->init = better_timeline_init;
  st->duplicate = better_timeline_duplicate;
  st->blend_write = better_timeline_space_blend_write;

  art = MEM_new_zeroed<ARegionType>("spacetype better timeline region");
  art->regionid = RGN_TYPE_WINDOW;
  art->keymapflag = ED_KEYMAP_UI;
  art->init = better_timeline_main_region_init;
  art->draw = better_timeline_main_region_draw;
  art->draw_overlay = better_timeline_main_region_draw_overlay;
  art->listener = better_timeline_main_region_listener;
  art->cursor = better_timeline_main_region_cursor;
  art->event_cursor = true;
  BLI_addhead(&st->regiontypes, art);

  art = MEM_new_zeroed<ARegionType>("spacetype better timeline header region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_HEADER;
  art->init = better_timeline_header_region_init;
  art->draw = better_timeline_header_region_draw;
  art->listener = better_timeline_header_region_listener;
  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(std::move(st));
}

}  // namespace blender
