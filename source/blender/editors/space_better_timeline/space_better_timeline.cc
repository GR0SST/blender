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
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"
#include "BKE_undo_system.hh"

#include "ED_anim_api.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_time_scrub_ui.hh"
#include "ED_undo.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "BLF_api.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "RNA_access.hh"

#include "BLO_read_write.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender {

static constexpr int BETTER_TIMELINE_MIN_ROWS = 6;
static constexpr int BETTER_TIMELINE_ROW_HEIGHT = 34;
static constexpr int BETTER_TIMELINE_PANEL_DEFAULT_WIDTH = 270;
static constexpr int BETTER_TIMELINE_PANEL_MIN_WIDTH = 180;
static constexpr int BETTER_TIMELINE_TIMELINE_MIN_WIDTH = 120;
static constexpr int BETTER_TIMELINE_DIVIDER_HIT_WIDTH = 5;
static constexpr int BETTER_TIMELINE_ADD_BUTTON_SIZE = 20;
static constexpr int BETTER_TIMELINE_ADD_BUTTON_MARGIN = 8;
static constexpr int BETTER_TIMELINE_SCROLLBAR_WIDTH = 12;
static constexpr int BETTER_TIMELINE_SCROLLBAR_MIN_THUMB_HEIGHT = 28;
static constexpr float BETTER_TIMELINE_REORDER_AUTOSCROLL_TIMER_STEP = 0.02f;
static constexpr const char *BETTER_TIMELINE_KEYMAP_NAME = "Better Timeline";

struct BetterTimelinePanelResizeData {
  int initial_mouse_x;
  int initial_panel_width;
};

struct BetterTimelineClipState {
  int scissor[4];
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

static BetterTimelineTrackDragVisualState g_better_timeline_track_drag_visual_state = {
    nullptr, nullptr, -1, false};

static void better_timeline_view_sync(ARegion *region,
                                      const Scene *scene,
                                      const SpaceBetterTimeline *sbetter_timeline);
static int better_timeline_content_height(const ARegion *region);
static float better_timeline_row_ymax(const ARegion *region,
                                      const SpaceBetterTimeline *sbetter_timeline,
                                      const int row_index);
static float better_timeline_row_ymin(const ARegion *region,
                                      const SpaceBetterTimeline *sbetter_timeline,
                                      const int row_index);
static void better_timeline_track_drag_visual_state_clear();

static bool better_timeline_track_is_selected(const BetterTimelineTrack *track)
{
  return track != nullptr && track->selected != 0;
}

static void better_timeline_track_set_selected(BetterTimelineTrack *track, const bool selected)
{
  if (track != nullptr) {
    track->selected = selected ? 1 : 0;
  }
}

static BetterTimelineTrack *better_timeline_track_at_index(SpaceBetterTimeline *sbetter_timeline,
                                                           const int track_index)
{
  if (sbetter_timeline == nullptr || track_index < 0) {
    return nullptr;
  }
  return static_cast<BetterTimelineTrack *>(BLI_findlink(&sbetter_timeline->tracks, track_index));
}

static const BetterTimelineTrack *better_timeline_track_at_index(
    const SpaceBetterTimeline *sbetter_timeline, const int track_index)
{
  if (sbetter_timeline == nullptr || track_index < 0) {
    return nullptr;
  }
  return static_cast<const BetterTimelineTrack *>(
      BLI_findlink(&sbetter_timeline->tracks, track_index));
}

static int better_timeline_track_count(const SpaceBetterTimeline *sbetter_timeline)
{
  return (sbetter_timeline != nullptr) ? BLI_listbase_count(&sbetter_timeline->tracks) : 0;
}

static int better_timeline_track_index_from_ptr(const SpaceBetterTimeline *sbetter_timeline,
                                                const BetterTimelineTrack *target_track)
{
  if (sbetter_timeline == nullptr || target_track == nullptr) {
    return -1;
  }

  int index = 0;
  for (const BetterTimelineTrack *track = static_cast<const BetterTimelineTrack *>(
           sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next, index++)
  {
    if (track == target_track) {
      return index;
    }
  }
  return -1;
}

static BetterTimelineTrack *better_timeline_track_create(const int track_name_index)
{
  auto *track = MEM_new<BetterTimelineTrack>(__func__);
  SNPRINTF(track->name, "Track%d", std::max(1, track_name_index));
  track->selected = 0;
  return track;
}

static void better_timeline_tracks_free(ListBaseT<BetterTimelineTrack> *tracks)
{
  if (tracks == nullptr) {
    return;
  }
  BLI_freelistN(tracks);
}

static void better_timeline_tracks_duplicate(ListBase *dst, const ListBase *src)
{
  BLI_listbase_clear(dst);
  if (src == nullptr) {
    return;
  }
  BLI_duplicatelist(dst, src);
}

static void better_timeline_state_restore(SpaceBetterTimeline *dst,
                                          const ListBase *tracks_src,
                                          const int selected_track_index,
                                          const int next_track_name_index,
                                          const int track_panel_width,
                                          const int track_scroll_offset)
{
  if (dst == nullptr) {
    return;
  }

  better_timeline_tracks_free(&dst->tracks);
  better_timeline_tracks_duplicate(&dst->tracks, tracks_src);
  dst->selected_track_index = selected_track_index;
  dst->next_track_name_index = std::max(1, next_track_name_index);
  dst->track_panel_width = track_panel_width;
  dst->track_scroll_offset = std::max(0, track_scroll_offset);
}

static bool better_timeline_has_selected_track(const SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return false;
  }
  for (const BetterTimelineTrack *track = static_cast<const BetterTimelineTrack *>(
           sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    if (better_timeline_track_is_selected(track)) {
      return true;
    }
  }
  return false;
}

static int better_timeline_selected_track_count(const SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return 0;
  }
  int count = 0;
  for (const BetterTimelineTrack *track = static_cast<const BetterTimelineTrack *>(
           sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    if (better_timeline_track_is_selected(track)) {
      count++;
    }
  }
  return count;
}

static void better_timeline_clear_selection(SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return;
  }
  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    better_timeline_track_set_selected(track, false);
  }
  sbetter_timeline->selected_track_index = -1;
}

static void better_timeline_tag_space_state_changed(bContext *C)
{
  Main *bmain = CTX_data_main(C);
  if (bmain != nullptr) {
    bmain->is_memfile_undo_flush_needed = true;
  }

  WM_event_add_notifier(C, NC_SCREEN | NA_EDITED, nullptr);
}

static bool better_timeline_undosys_poll(bContext *C)
{
  if (C == nullptr) {
    return false;
  }
  const ScrArea *area = CTX_wm_area(C);
  const SpaceLink *space_link = CTX_wm_space_data(C);
  return area != nullptr && space_link != nullptr && area->spacetype == SPACE_BETTER_TIMELINE &&
         space_link->spacetype == SPACE_BETTER_TIMELINE;
}

static bool better_timeline_undosys_step_encode(bContext *C, Main * /*bmain*/, UndoStep *us_p)
{
  auto *us = reinterpret_cast<BetterTimelineUndoStep *>(us_p);
  auto *sbetter_timeline = reinterpret_cast<SpaceBetterTimeline *>(CTX_wm_space_data(C));
  if (sbetter_timeline == nullptr) {
    return false;
  }

  us->space = sbetter_timeline;
  better_timeline_tracks_duplicate(&us->tracks, &sbetter_timeline->tracks);
  us->selected_track_index = sbetter_timeline->selected_track_index;
  us->next_track_name_index = sbetter_timeline->next_track_name_index;
  us->track_panel_width = sbetter_timeline->track_panel_width;
  us->track_scroll_offset = sbetter_timeline->track_scroll_offset;
  return true;
}

static void better_timeline_undosys_step_decode(
    bContext *C, Main * /*bmain*/, UndoStep *us_p, const eUndoStepDir /*dir*/, bool /*is_final*/)
{
  auto *us = reinterpret_cast<BetterTimelineUndoStep *>(us_p);
  if (us->space == nullptr) {
    return;
  }

  better_timeline_state_restore(us->space,
                                &us->tracks,
                                us->selected_track_index,
                                us->next_track_name_index,
                                us->track_panel_width,
                                us->track_scroll_offset);
  better_timeline_track_drag_visual_state_clear();
  WM_event_add_notifier(C, NC_SCREEN | NA_EDITED, nullptr);
}

static void better_timeline_undosys_step_free(UndoStep *us_p)
{
  auto *us = reinterpret_cast<BetterTimelineUndoStep *>(us_p);
  better_timeline_tracks_free(reinterpret_cast<ListBaseT<BetterTimelineTrack> *>(&us->tracks));
}

static void better_timeline_select_only_track(SpaceBetterTimeline *sbetter_timeline,
                                              const int track_index)
{
  better_timeline_clear_selection(sbetter_timeline);

  BetterTimelineTrack *track = better_timeline_track_at_index(sbetter_timeline, track_index);
  if (track != nullptr) {
    better_timeline_track_set_selected(track, true);
    sbetter_timeline->selected_track_index = track_index;
  }
}

static int better_timeline_first_selected_track_index(const SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return -1;
  }

  int index = 0;
  for (const BetterTimelineTrack *track = static_cast<const BetterTimelineTrack *>(
           sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    if (better_timeline_track_is_selected(track)) {
      return index;
    }
    index++;
  }
  return -1;
}

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

static void better_timeline_view2d_update_old_window(ARegion *region,
                                                     const SpaceBetterTimeline *sbetter_timeline)
{
  View2D *v2d = &region->v2d;
  const int panel_width = better_timeline_left_panel_width(region, sbetter_timeline);
  const int mask_width = std::max(1, region->winx - panel_width);
  const int mask_height = std::max(1, better_timeline_content_height(region));

  v2d->oldwinx = short(mask_width + 1);
  v2d->oldwiny = short(mask_height + 1);
}

static void better_timeline_view2d_apply_mask(ARegion *region,
                                              View2D *v2d,
                                              const SpaceBetterTimeline *sbetter_timeline)
{
  v2d->mask.xmin = better_timeline_left_panel_width(region, sbetter_timeline);
  v2d->mask.xmax = std::max(v2d->mask.xmin, int(region->winx) - 1);
  v2d->mask.ymin = 0;
  v2d->mask.ymax = std::max(v2d->mask.ymin, better_timeline_content_height(region) - 1);
}

static void better_timeline_clip_begin(const ARegion *region,
                                       const rcti &rect,
                                       BetterTimelineClipState *r_state)
{
  UNUSED_VARS(region);
  GPU_scissor_get(r_state->scissor);
  GPU_scissor(rect.xmin, rect.ymin, BLI_rcti_size_x(&rect), BLI_rcti_size_y(&rect));
}

static void better_timeline_clip_end(const BetterTimelineClipState &state)
{
  GPU_scissor(UNPACK4(state.scissor));
}

static void better_timeline_view_ortho(const View2D *v2d)
{
  rctf curmasked = v2d->cur;
  const int sizex = BLI_rcti_size_x(&v2d->mask);
  const int sizey = BLI_rcti_size_y(&v2d->mask);
  const float eps = 0.001f;
  float xofs = 0.0f;
  float yofs = 0.0f;

  if (sizex > 0) {
    xofs = eps * BLI_rctf_size_x(&v2d->cur) / sizex;
  }
  if (sizey > 0) {
    yofs = eps * BLI_rctf_size_y(&v2d->cur) / sizey;
  }

  if (sizex > 0 && sizey > 0) {
    const float dx = BLI_rctf_size_x(&v2d->cur) / float(sizex + 1);
    const float dy = BLI_rctf_size_y(&v2d->cur) / float(sizey + 1);

    if (v2d->mask.xmin != 0) {
      curmasked.xmin -= dx * float(v2d->mask.xmin);
    }
    if (v2d->mask.xmax + 1 != v2d->winx) {
      curmasked.xmax += dx * float(v2d->winx - v2d->mask.xmax - 1);
    }

    if (v2d->mask.ymin != 0) {
      curmasked.ymin -= dy * float(v2d->mask.ymin);
    }
    if (v2d->mask.ymax + 1 != v2d->winy) {
      curmasked.ymax += dy * float(v2d->winy - v2d->mask.ymax - 1);
    }
  }

  BLI_rctf_translate(&curmasked, -xofs, -yofs);

  if (v2d->flag & V2D_PIXELOFS_X) {
    curmasked.xmin = floorf(curmasked.xmin) - (eps + xofs);
    curmasked.xmax = floorf(curmasked.xmax) - (eps + xofs);
  }
  if (v2d->flag & V2D_PIXELOFS_Y) {
    curmasked.ymin = floorf(curmasked.ymin) - (eps + yofs);
    curmasked.ymax = floorf(curmasked.ymax) - (eps + yofs);
  }

  wmOrtho2(curmasked.xmin, curmasked.xmax, curmasked.ymin, curmasked.ymax);
}

static void better_timeline_draw_grid_x_frames(const View2D *v2d, const Scene *scene)
{
  UNUSED_VARS(scene);
  const int pixel_width = BLI_rcti_size_x(&v2d->mask) + 1;
  const float view_width = BLI_rctf_size_x(&v2d->cur);
  const float pixels_per_frame = pixel_width / std::max(1.0f, view_width);
  const float min_major_px = 35.0f;
  const float min_minor_px = 14.0f;
  float major_line_distance = 1.0f;
  while ((major_line_distance * pixels_per_frame) < min_major_px) {
    major_line_distance *= 2.0f;
  }

  auto draw_lines = [&](const float line_distance, const uchar color[3]) {
    const float start_value = floorf(v2d->cur.xmin / line_distance) * line_distance;
    const uint steps = uint(ceilf((v2d->cur.xmax - start_value) / line_distance)) + 1;

    GPUVertFormat *format = immVertexFormat();
    const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor3ubv(color);
    immBegin(GPU_PRIM_LINES, steps * 2);
    for (uint i = 0; i < steps; i++) {
      const float xpos = start_value + i * line_distance;
      immVertex2f(pos, xpos, v2d->cur.ymin);
      immVertex2f(pos, xpos, v2d->cur.ymax);
    }
    immEnd();
    immUnbindProgram();
  };

  GPU_matrix_push_projection();
  better_timeline_view_ortho(v2d);

  uchar minor_color[3];
  ui::theme::get_color_shade_3ubv(TH_GRID, 16, minor_color);
  if ((major_line_distance * 0.5f * pixels_per_frame) >= min_minor_px && major_line_distance > 1.0f) {
    draw_lines(major_line_distance / 2.0f, minor_color);
  }

  uchar major_color[3];
  ui::theme::get_color_3ubv(TH_GRID, major_color);
  draw_lines(major_line_distance, major_color);

  GPU_matrix_pop_projection();
}

static rcti better_timeline_body_rect(const ARegion *region,
                                      const SpaceBetterTimeline *sbetter_timeline)
{
  rcti rect{};
  rect.xmin = better_timeline_left_panel_width(region, sbetter_timeline);
  rect.xmax = std::max(rect.xmin + 1, int(region->winx));
  rect.ymin = 0;
  rect.ymax = std::max(1, better_timeline_content_height(region));
  return rect;
}

static rcti better_timeline_scrub_rect(const ARegion *region,
                                       const SpaceBetterTimeline *sbetter_timeline)
{
  rcti rect = better_timeline_body_rect(region, sbetter_timeline);
  rect.ymin = rect.ymax;
  rect.ymax = std::max(rect.ymin + 1, int(region->winy));
  return rect;
}

static rcti better_timeline_track_header_rect(const ARegion *region,
                                              const SpaceBetterTimeline *sbetter_timeline)
{
  const int left_panel_width = better_timeline_left_panel_width(region, sbetter_timeline);
  const int content_top = better_timeline_content_height(region);

  rcti rect{};
  rect.xmin = 0;
  rect.xmax = left_panel_width;
  rect.ymin = content_top;
  rect.ymax = std::max(content_top + 1, int(region->winy));
  return rect;
}

static rcti better_timeline_add_button_rect(const ARegion *region,
                                            const SpaceBetterTimeline *sbetter_timeline)
{
  const rcti header_rect = better_timeline_track_header_rect(region, sbetter_timeline);
  const int button_size = int(BETTER_TIMELINE_ADD_BUTTON_SIZE * UI_SCALE_FAC);
  const int button_margin = int(BETTER_TIMELINE_ADD_BUTTON_MARGIN * UI_SCALE_FAC);
  const int header_height = BLI_rcti_size_y(&header_rect);
  const int button_ymin = header_rect.ymin + std::max(0, (header_height - button_size) / 2);

  rcti rect{};
  rect.xmin = header_rect.xmin + button_margin;
  rect.xmax = rect.xmin + button_size;
  rect.ymin = button_ymin;
  rect.ymax = button_ymin + button_size;
  return rect;
}

static bool better_timeline_is_in_add_button(const ARegion *region,
                                             const SpaceBetterTimeline *sbetter_timeline,
                                             const int region_x,
                                             const int region_y)
{
  const rcti rect = better_timeline_add_button_rect(region, sbetter_timeline);
  return BLI_rcti_isect_pt(&rect, region_x, region_y);
}

static bool better_timeline_scrub_event_in_region(const ScrArea *area,
                                                  const ARegion *region,
                                                  const wmEvent *event)
{
  if (area == nullptr || region == nullptr || event == nullptr ||
      area->spacetype != SPACE_BETTER_TIMELINE)
  {
    return false;
  }

  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(area->spacedata.first);
  const rcti scrub_rect = better_timeline_scrub_rect(region, sbetter_timeline);
  const int xmin = region->winrct.xmin + scrub_rect.xmin;
  const int xmax = region->winrct.xmin + scrub_rect.xmax;
  const int ymin = region->winrct.ymin + scrub_rect.ymin;
  const int ymax = region->winrct.ymin + scrub_rect.ymax;

  return event->xy[0] >= xmin && event->xy[0] < xmax && event->xy[1] >= ymin &&
         event->xy[1] < ymax;
}

static bool better_timeline_time_scrub_event_in_region_poll(const wmWindow * /*win*/,
                                                            const ScrArea *area,
                                                            const ARegion *region,
                                                            const wmEvent *event)
{
  return better_timeline_scrub_event_in_region(area, region, event);
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

static int better_timeline_track_scroll_max(const ARegion *region,
                                            const SpaceBetterTimeline *sbetter_timeline)
{
  const int total_track_height = better_timeline_track_count(sbetter_timeline) * BETTER_TIMELINE_ROW_HEIGHT;
  return std::max(0, total_track_height - better_timeline_content_height(region));
}

static int better_timeline_track_scroll_offset(const ARegion *region,
                                               const SpaceBetterTimeline *sbetter_timeline)
{
  return std::clamp((sbetter_timeline != nullptr) ? sbetter_timeline->track_scroll_offset : 0,
                    0,
                    better_timeline_track_scroll_max(region, sbetter_timeline));
}

static bool better_timeline_track_scrollbar_visible(const ARegion *region,
                                                    const SpaceBetterTimeline *sbetter_timeline)
{
  return better_timeline_track_scroll_max(region, sbetter_timeline) > 0;
}

static rcti better_timeline_track_scrollbar_rect(const ARegion *region,
                                                 const SpaceBetterTimeline *sbetter_timeline)
{
  const int scrollbar_width = int(BETTER_TIMELINE_SCROLLBAR_WIDTH * UI_SCALE_FAC);
  const int content_top = better_timeline_content_height(region);

  rcti rect{};
  rect.xmax = region->winx;
  rect.xmin = std::max(0, rect.xmax - scrollbar_width);
  rect.ymin = 0;
  rect.ymax = content_top;

  if (!better_timeline_track_scrollbar_visible(region, sbetter_timeline)) {
    rect.xmin = rect.xmax;
  }

  return rect;
}

static rcti better_timeline_track_scrollbar_thumb_rect(const ARegion *region,
                                                       const SpaceBetterTimeline *sbetter_timeline)
{
  const rcti scrollbar_rect = better_timeline_track_scrollbar_rect(region, sbetter_timeline);
  const int scrollbar_height = std::max(1, BLI_rcti_size_y(&scrollbar_rect));
  const int visible_height = better_timeline_content_height(region);
  const int total_track_height = std::max(visible_height,
                                          better_timeline_track_count(sbetter_timeline) *
                                              BETTER_TIMELINE_ROW_HEIGHT);
  const int scroll_max = better_timeline_track_scroll_max(region, sbetter_timeline);
  const int thumb_height = std::clamp(
      int(std::round(float(visible_height) * float(scrollbar_height) / float(total_track_height))),
      int(BETTER_TIMELINE_SCROLLBAR_MIN_THUMB_HEIGHT * UI_SCALE_FAC),
      scrollbar_height);
  const int travel = std::max(0, scrollbar_height - thumb_height);
  const float scroll_ratio = (scroll_max > 0) ?
                                 float(better_timeline_track_scroll_offset(region, sbetter_timeline)) /
                                     float(scroll_max) :
                                 0.0f;
  const int thumb_top_offset = int(std::round(scroll_ratio * travel));

  rcti rect{};
  rect.xmin = scrollbar_rect.xmin;
  rect.xmax = scrollbar_rect.xmax;
  rect.ymax = scrollbar_rect.ymax - thumb_top_offset;
  rect.ymin = rect.ymax - thumb_height;
  return rect;
}

static bool better_timeline_is_in_track_scrollbar(const ARegion *region,
                                                  const SpaceBetterTimeline *sbetter_timeline,
                                                  const int region_x,
                                                  const int region_y)
{
  if (!better_timeline_track_scrollbar_visible(region, sbetter_timeline)) {
    return false;
  }

  const rcti rect = better_timeline_track_scrollbar_rect(region, sbetter_timeline);
  return BLI_rcti_isect_pt(&rect, region_x, region_y);
}

static bool better_timeline_is_in_track_list_pane(const ARegion *region,
                                                  const SpaceBetterTimeline *sbetter_timeline,
                                                  const int region_x)
{
  return region_x < better_timeline_left_panel_width(region, sbetter_timeline);
}

static float better_timeline_row_ymax(const ARegion *region,
                                      const SpaceBetterTimeline *sbetter_timeline,
                                      const int row_index)
{
  const int content_top = better_timeline_content_height(region);
  const int scroll_offset = better_timeline_track_scroll_offset(region, sbetter_timeline);
  return float(content_top + scroll_offset - (row_index * BETTER_TIMELINE_ROW_HEIGHT));
}

static float better_timeline_row_ymin(const ARegion *region,
                                      const SpaceBetterTimeline *sbetter_timeline,
                                      const int row_index)
{
  return better_timeline_row_ymax(region, sbetter_timeline, row_index) - BETTER_TIMELINE_ROW_HEIGHT;
}

static bool better_timeline_row_is_visible(const ARegion *region,
                                           const SpaceBetterTimeline *sbetter_timeline,
                                           const int row_index)
{
  const float y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
  const float y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
  const int content_top = better_timeline_content_height(region);
  return y_max > 0.0f && y_min < content_top;
}

static int better_timeline_track_from_region_y(const ARegion *region,
                                               const SpaceBetterTimeline *sbetter_timeline,
                                               const int region_y)
{
  const int content_top = better_timeline_content_height(region);
  const int scroll_offset = better_timeline_track_scroll_offset(region, sbetter_timeline);
  const int track_count = better_timeline_track_count(sbetter_timeline);

  if (region_y >= content_top) {
    return -1;
  }
  if (region_y < 0) {
    return -1;
  }

  const int track_index = (content_top + scroll_offset - region_y - 1) / BETTER_TIMELINE_ROW_HEIGHT;
  return (track_index >= 0 && track_index < track_count) ? track_index : -1;
}

static int better_timeline_track_insertion_index_from_region_y(
    const ARegion *region, const SpaceBetterTimeline *sbetter_timeline, const int region_y)
{
  const int track_count = better_timeline_track_count(sbetter_timeline);
  if (track_count == 0) {
    return 0;
  }

  const int content_top = better_timeline_content_height(region);
  if (region_y >= content_top) {
    return 0;
  }
  if (region_y < 0) {
    return track_count;
  }

  const int track_index = better_timeline_track_from_region_y(region, sbetter_timeline, region_y);
  if (track_index < 0) {
    return track_count;
  }

  const float row_y_center = (better_timeline_row_ymin(region, sbetter_timeline, track_index) +
                              better_timeline_row_ymax(region, sbetter_timeline, track_index)) *
                             0.5f;
  return (float(region_y) >= row_y_center) ? track_index : track_index + 1;
}

static bool better_timeline_reorder_track_to_index(SpaceBetterTimeline *sbetter_timeline,
                                                   BetterTimelineTrack *track,
                                                   const int target_index)
{
  const int track_count = better_timeline_track_count(sbetter_timeline);
  if (sbetter_timeline == nullptr || track == nullptr || track_count < 2 || target_index < 0 ||
      target_index >= track_count)
  {
    return false;
  }

  const int current_index = better_timeline_track_index_from_ptr(sbetter_timeline, track);
  if (current_index < 0 || current_index == target_index) {
    return false;
  }

  if (target_index < current_index) {
    BetterTimelineTrack *insert_before = better_timeline_track_at_index(sbetter_timeline,
                                                                        target_index);
    if (insert_before == nullptr || insert_before == track) {
      return false;
    }
    BLI_remlink(&sbetter_timeline->tracks, track);
    BLI_insertlinkbefore(&sbetter_timeline->tracks, insert_before, track);
    return true;
  }

  BetterTimelineTrack *insert_after = better_timeline_track_at_index(sbetter_timeline,
                                                                     target_index);
  if (insert_after == nullptr || insert_after == track) {
    return false;
  }
  BLI_remlink(&sbetter_timeline->tracks, track);
  BLI_insertlinkafter(&sbetter_timeline->tracks, insert_after, track);
  return true;
}

static bool better_timeline_reorder_selected_tracks_to_insertion_index(
    SpaceBetterTimeline *sbetter_timeline, const int insertion_index)
{
  const int track_count = better_timeline_track_count(sbetter_timeline);
  const int selected_track_count = better_timeline_selected_track_count(sbetter_timeline);
  if (sbetter_timeline == nullptr || track_count < 2 || selected_track_count < 1) {
    return false;
  }
  if (selected_track_count == 1) {
    BetterTimelineTrack *selected_track = static_cast<BetterTimelineTrack *>(
        sbetter_timeline->tracks.first);
    while (selected_track != nullptr && !better_timeline_track_is_selected(selected_track)) {
      selected_track = selected_track->next;
    }
    if (selected_track == nullptr) {
      return false;
    }

    const int current_index = better_timeline_track_index_from_ptr(sbetter_timeline, selected_track);
    if (current_index < 0) {
      return false;
    }

    const int target_index = std::clamp(
        (insertion_index <= current_index) ? insertion_index : insertion_index - 1,
        0,
        std::max(0, track_count - 1));
    return better_timeline_reorder_track_to_index(sbetter_timeline, selected_track, target_index);
  }

  int selected_before_insertion = 0;
  int index = 0;
  for (const BetterTimelineTrack *track = static_cast<const BetterTimelineTrack *>(
           sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next, index++)
  {
    if (index >= insertion_index) {
      break;
    }
    if (better_timeline_track_is_selected(track)) {
      selected_before_insertion++;
    }
  }

  const int remaining_track_count = track_count - selected_track_count;
  const int adjusted_insertion_index = std::clamp(
      insertion_index - selected_before_insertion, 0, remaining_track_count);

  ListBase moved_tracks{};
  BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
  while (track != nullptr) {
    BetterTimelineTrack *next_track = track->next;
    if (better_timeline_track_is_selected(track)) {
      BLI_remlink(&sbetter_timeline->tracks, track);
      BLI_addtail(&moved_tracks, track);
    }
    track = next_track;
  }

  if (BLI_listbase_is_empty(&moved_tracks)) {
    return false;
  }

  BetterTimelineTrack *insert_before = better_timeline_track_at_index(
      sbetter_timeline, adjusted_insertion_index);
  if (insert_before == nullptr) {
    BLI_movelisttolist(&sbetter_timeline->tracks, &moved_tracks);
    return true;
  }

  track = static_cast<BetterTimelineTrack *>(moved_tracks.first);
  while (track != nullptr) {
    BetterTimelineTrack *next_track = track->next;
    BLI_remlink(&moved_tracks, track);
    BLI_insertlinkbefore(&sbetter_timeline->tracks, insert_before, track);
    track = next_track;
  }
  return true;
}

static int better_timeline_track_reorder_autoscroll_step(const ARegion *region,
                                                         const SpaceBetterTimeline *sbetter_timeline,
                                                         const int region_y)
{
  const int scroll_max = better_timeline_track_scroll_max(region, sbetter_timeline);
  if (scroll_max == 0) {
    return 0;
  }

  const int content_top = better_timeline_content_height(region);
  const int edge_size = std::max(12, int(24.0f * UI_SCALE_FAC));
  const int scroll_step = std::max(1, BETTER_TIMELINE_ROW_HEIGHT / 4);

  if (region_y >= content_top - edge_size) {
    return -scroll_step;
  }
  if (region_y <= edge_size) {
    return scroll_step;
  }
  return 0;
}

static bool better_timeline_track_reorder_autoscroll_apply(
    const ARegion *region, SpaceBetterTimeline *sbetter_timeline, const int region_y)
{
  const int scroll_step = better_timeline_track_reorder_autoscroll_step(
      region, sbetter_timeline, region_y);
  if (scroll_step == 0) {
    return false;
  }

  const int new_scroll_offset = std::clamp(
      better_timeline_track_scroll_offset(region, sbetter_timeline) + scroll_step,
      0,
      better_timeline_track_scroll_max(region, sbetter_timeline));
  if (new_scroll_offset == sbetter_timeline->track_scroll_offset) {
    return false;
  }

  sbetter_timeline->track_scroll_offset = new_scroll_offset;
  return true;
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

static void better_timeline_track_drag_visual_state_update(
    const ARegion *region, const BetterTimelineTrack *dragged_track, const int insertion_index)
{
  g_better_timeline_track_drag_visual_state.region = region;
  g_better_timeline_track_drag_visual_state.dragged_track = dragged_track;
  g_better_timeline_track_drag_visual_state.insertion_index = insertion_index;
  g_better_timeline_track_drag_visual_state.active = true;
}

static void better_timeline_track_drag_visual_state_clear()
{
  g_better_timeline_track_drag_visual_state.region = nullptr;
  g_better_timeline_track_drag_visual_state.dragged_track = nullptr;
  g_better_timeline_track_drag_visual_state.insertion_index = -1;
  g_better_timeline_track_drag_visual_state.active = false;
}

static float better_timeline_track_insertion_y(const ARegion *region,
                                               const SpaceBetterTimeline *sbetter_timeline,
                                               const int insertion_index)
{
  const int track_count = better_timeline_track_count(sbetter_timeline);
  const int clamped_index = std::clamp(insertion_index, 0, track_count);

  if (clamped_index <= 0) {
    return float(better_timeline_content_height(region));
  }
  if (clamped_index >= track_count) {
    return (track_count > 0) ? better_timeline_row_ymin(region, sbetter_timeline, track_count - 1) :
                               0.0f;
  }
  return better_timeline_row_ymax(region, sbetter_timeline, clamped_index);
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

  if (better_timeline_scrub_event_in_region(area, region, event)) {
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
    if (!shift && !oskey && better_timeline_has_selected_track(sbetter_timeline)) {
      better_timeline_clear_selection(sbetter_timeline);
      ED_area_tag_redraw(area);
      return OPERATOR_FINISHED;
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
                                       sbetter_timeline->selected_track_index == clicked_track_index;
    if (already_only_selected) {
      return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
    }

    if (clicked_in_track_list && clicked_track_selected && selected_track_count > 1) {
      return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
    }

    better_timeline_select_only_track(sbetter_timeline, clicked_track_index);
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
          const bool changed = better_timeline_reorder_selected_tracks_to_insertion_index(
              sbetter_timeline, reorder_data->current_insertion_index);
          if (changed) {
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

static wmOperatorStatus better_timeline_add_track_exec(bContext *C, wmOperator * /*op*/)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  BetterTimelineTrack *track = better_timeline_track_create(sbetter_timeline->next_track_name_index);
  BLI_addtail(&sbetter_timeline->tracks, track);
  sbetter_timeline->next_track_name_index = std::max(1, sbetter_timeline->next_track_name_index + 1);
  better_timeline_select_only_track(sbetter_timeline, better_timeline_track_count(sbetter_timeline) - 1);
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

  return better_timeline_add_track_exec(C, op);
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
  if (better_timeline_has_selected_track(sbetter_timeline)) {
    return OPERATOR_CANCELLED;
  }

  ui::PopupMenu *pup = ui::popup_menu_begin(C, "Add Track", ICON_NONE);
  ui::Layout &layout = *popup_menu_layout(pup);
  layout.op("BETTER_TIMELINE_OT_add_track", "Test Track", ICON_NONE);
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

static bool better_timeline_track_requires_delete_confirm(
    const SpaceBetterTimeline * /*sbetter_timeline*/)
{
  /* Placeholder tracks currently have no clips, so they can be removed immediately.
   * Once clip data exists, this should return true for non-empty tracks. */
  return false;
}

static wmOperatorStatus better_timeline_delete_track_exec(bContext *C, wmOperator * /*op*/)
{
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  if (!better_timeline_has_selected_track(sbetter_timeline)) {
    return OPERATOR_CANCELLED;
  }

  BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
  while (track != nullptr) {
    BetterTimelineTrack *track_next = track->next;
    if (better_timeline_track_is_selected(track)) {
      BLI_remlink(&sbetter_timeline->tracks, track);
      MEM_delete(track);
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
  return better_timeline_has_selected_track(sbetter_timeline);
}

static wmOperatorStatus better_timeline_clear_selection_exec(bContext *C, wmOperator * /*op*/)
{
  ScrArea *area = CTX_wm_area(C);
  auto *sbetter_timeline = static_cast<SpaceBetterTimeline *>(area->spacedata.first);

  better_timeline_clear_selection(sbetter_timeline);
  ED_area_tag_redraw(area);
  return OPERATOR_FINISHED;
}

static void BETTER_TIMELINE_OT_clear_selection(wmOperatorType *ot)
{
  ot->name = "Clear Better Timeline Selection";
  ot->idname = "BETTER_TIMELINE_OT_clear_selection";
  ot->description = "Clear selected tracks in Better Timeline";

  ot->exec = better_timeline_clear_selection_exec;
  ot->poll = better_timeline_clear_selection_poll;

  ot->flag = OPTYPE_INTERNAL;
}

static bool better_timeline_scroll_tracks_poll(bContext *C)
{
  return better_timeline_track_select_poll(C);
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
  return better_timeline_track_select_poll(C);
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
      const int travel = std::max(1, BLI_rcti_size_y(&scrollbar_rect) - BLI_rcti_size_y(&thumb_rect));
      const int mouse_delta = drag_data->initial_mouse_y - event->mval[1];
      const int scroll_delta = int(std::round(float(mouse_delta) * float(scroll_max) / float(travel)));
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

  if (!better_timeline_is_in_track_scrollbar(region, sbetter_timeline, event->mval[0], event->mval[1])) {
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
    const int travel = std::max(1, BLI_rcti_size_y(&scrollbar_rect) - BLI_rcti_size_y(&thumb_rect));
    const int click_top_offset = std::clamp(scrollbar_rect.ymax - event->mval[1] - (BLI_rcti_size_y(&thumb_rect) / 2),
                                            0,
                                            travel);
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
  bool has_track_select_shift = false;
  bool has_track_select_oskey = false;
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
  bool has_undo = false;
  bool has_redo = false;

  for (wmKeyMapItem *kmi = static_cast<wmKeyMapItem *>(keymap->items.first); kmi != nullptr;
       kmi = kmi->next)
  {
    if (STREQ(kmi->idname, "BETTER_TIMELINE_OT_add_track")) {
      has_add_track = true;
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
    else if (STREQ(kmi->idname, "ED_OT_undo")) {
      has_undo = (kmi->type == EVT_ZKEY && kmi->val == KM_PRESS && kmi->oskey == KM_MOD_HELD &&
                  kmi->shift == KM_NOTHING);
    }
    else if (STREQ(kmi->idname, "ED_OT_redo")) {
      has_redo = (kmi->type == EVT_ZKEY && kmi->val == KM_PRESS && kmi->oskey == KM_MOD_HELD &&
                  kmi->shift == KM_MOD_HELD);
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
    params.value = KM_PRESS;
    params.modifier = 0;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "BETTER_TIMELINE_OT_add_track", &params);
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
  const float total_rows = float(std::max(
      better_timeline_row_count(region), better_timeline_track_count(sbetter_timeline)));
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
  v2d->keepzoom = V2D_LOCKZOOM_Y | V2D_LIMITZOOM;
  v2d->keepofs = V2D_KEEPOFS_Y;
  v2d->align = V2D_ALIGN_NO_POS_Y;
  v2d->scroll = 0;

  better_timeline_view2d_apply_mask(region, v2d, sbetter_timeline);

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
  better_timeline_view2d_apply_mask(region, v2d, sbetter_timeline);
}

static SpaceLink *better_timeline_create(const ScrArea * /*area*/, const Scene * /*scene*/)
{
  ARegion *region;
  SpaceBetterTimeline *sbetter_timeline;

  sbetter_timeline = MEM_new<SpaceBetterTimeline>("init better timeline");
  sbetter_timeline->spacetype = SPACE_BETTER_TIMELINE;
  sbetter_timeline->selected_track_index = -1;
  sbetter_timeline->next_track_name_index = 1;
  sbetter_timeline->track_panel_width = 0;
  sbetter_timeline->track_scroll_offset = 0;

  region = BKE_area_region_new();
  BLI_addtail(&sbetter_timeline->regionbase, region);
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;

  region = BKE_area_region_new();
  BLI_addtail(&sbetter_timeline->regionbase, region);
  region->regiontype = RGN_TYPE_WINDOW;

  return reinterpret_cast<SpaceLink *>(sbetter_timeline);
}

static void better_timeline_free(SpaceLink *sl)
{
  auto *sbetter_timeline = reinterpret_cast<SpaceBetterTimeline *>(sl);
  better_timeline_tracks_free(&sbetter_timeline->tracks);
}

static void better_timeline_init(wmWindowManager * /*wm*/, ScrArea * /*area*/) {}

static SpaceLink *better_timeline_duplicate(SpaceLink *sl)
{
  SpaceBetterTimeline *sbetter_timeline = MEM_dupalloc(
      reinterpret_cast<SpaceBetterTimeline *>(sl));
  BLI_duplicatelist(&sbetter_timeline->tracks,
                    &reinterpret_cast<SpaceBetterTimeline *>(sl)->tracks);
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
      &region->runtime->handlers, keymap, better_timeline_time_scrub_event_in_region_poll);

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
  if (better_timeline_is_in_track_scrollbar(region,
                                            sbetter_timeline,
                                            region_x,
                                            win->runtime->eventstate->xy[1] - region->winrct.ymin))
  {
    WM_cursor_set(win, WM_CURSOR_Y_MOVE);
    return;
  }

  WM_cursor_set(win, WM_CURSOR_DEFAULT);
}

static void better_timeline_draw_layout_overlay(const ARegion *region,
                                                const SpaceBetterTimeline *sbetter_timeline)
{
  const int left_panel_width = better_timeline_left_panel_width(region, sbetter_timeline);
  const int content_top = better_timeline_content_height(region);
  const int track_count = better_timeline_track_count(sbetter_timeline);
  const bool drag_active = g_better_timeline_track_drag_visual_state.active &&
                           g_better_timeline_track_drag_visual_state.region == region;
  const BetterTimelineTrack *dragged_track = drag_active ?
                                                 g_better_timeline_track_drag_visual_state.dragged_track :
                                                 nullptr;
  const int dragged_track_index = drag_active ?
                                      better_timeline_track_index_from_ptr(sbetter_timeline,
                                                                           dragged_track) :
                                      -1;
  float drag_color[3] = {0.0f, 0.0f, 0.0f};
  const float insertion_color[3] = {0.29f, 0.58f, 0.96f};
  ui::theme::get_color_3fv(TH_SELECT, drag_color);
  const rcti add_button_rect = better_timeline_add_button_rect(region, sbetter_timeline);
  const rcti content_rect = {0, region->winx, 0, content_top};
  int visible_separator_count = 0;
  for (int row_index = 0; row_index <= track_count; row_index++) {
    const float y = better_timeline_row_ymax(region, sbetter_timeline, row_index);
    if (y >= 0.0f && y <= content_top) {
      visible_separator_count++;
    }
  }

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

  immUniformColor4f(0.22f, 0.22f, 0.22f, 1.0f);
  immRectf(pos,
           float(add_button_rect.xmin),
           float(add_button_rect.ymin),
           float(add_button_rect.xmax),
           float(add_button_rect.ymax));

  immUniformColor4f(0.17f, 0.17f, 0.17f, 1.0f);
  immRectf(pos, 0.0f, 0.0f, float(left_panel_width), float(content_top));

  BetterTimelineClipState content_clip_state;
  better_timeline_clip_begin(region, content_rect, &content_clip_state);
  for (int row_index = 0; row_index < track_count; row_index++) {
    if (!better_timeline_row_is_visible(region, sbetter_timeline, row_index)) {
      continue;
    }
    const float y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
    const float y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);

    const BetterTimelineTrack *track = better_timeline_track_at_index(sbetter_timeline, row_index);
    if (better_timeline_track_is_selected(track)) {
      immUniformColor4f(0.25f, 0.40f, 0.72f, 0.92f);
      immRectf(pos, 0.0f, y_min, float(left_panel_width), y_max);

      immUniformColor4f(0.25f, 0.40f, 0.72f, 0.20f);
      immRectf(pos, float(left_panel_width), y_min, float(region->winx), y_max);

      if (row_index == dragged_track_index) {
        immUniformColor4f(drag_color[0], drag_color[1], drag_color[2], 0.14f);
        immRectf(pos, 0.0f, y_min, float(left_panel_width), y_max);

        immUniformColor4f(drag_color[0], drag_color[1], drag_color[2], 0.08f);
        immRectf(pos, float(left_panel_width), y_min, float(region->winx), y_max);
      }
    }
    else {
      /* The track list is its own opaque pane; keep the timeline content from bleeding through it.
       */
      immUniformColor4f(0.19f, 0.19f, 0.19f, 1.0f);
      immRectf(pos, 0.0f, y_min, float(left_panel_width), y_max);

      immUniformColor4f(1.0f, 1.0f, 1.0f, 0.055f);
      immRectf(pos, float(left_panel_width), y_min, float(region->winx), y_max);
    }
  }
  better_timeline_clip_end(content_clip_state);

  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.08f);
  GPU_line_width(1.0f);
  better_timeline_clip_begin(region, content_rect, &content_clip_state);
  immBegin(GPU_PRIM_LINES, (visible_separator_count * 4) + 2);
  for (int row_index = 0; row_index <= track_count; row_index++) {
    const float y = better_timeline_row_ymax(region, sbetter_timeline, row_index);
    if (y < 0.0f || y > content_top) {
      continue;
    }
    immVertex2f(pos, 0.0f, y);
    immVertex2f(pos, float(left_panel_width), y);
    immVertex2f(pos, float(left_panel_width), y);
    immVertex2f(pos, float(region->winx), y);
  }
  immVertex2f(pos, float(left_panel_width), 0.0f);
  immVertex2f(pos, float(left_panel_width), float(region->winy));
  immEnd();

  if (drag_active && dragged_track_index >= 0) {
    const float insertion_y = better_timeline_track_insertion_y(
        region, sbetter_timeline, g_better_timeline_track_drag_visual_state.insertion_index);

    immUniformColor4f(
        insertion_color[0], insertion_color[1], insertion_color[2], 0.95f);
    GPU_line_width(3.0f);
    immBegin(GPU_PRIM_LINES, 6);
    immVertex2f(pos, 10.0f, insertion_y);
    immVertex2f(pos, float(region->winx - 10), insertion_y);
    immVertex2f(pos, 10.0f, insertion_y - 7.0f);
    immVertex2f(pos, 10.0f, insertion_y + 7.0f);
    immVertex2f(pos, float(region->winx - 10), insertion_y - 7.0f);
    immVertex2f(pos, float(region->winx - 10), insertion_y + 7.0f);
    immEnd();
  }
  better_timeline_clip_end(content_clip_state);

  immUniformColor4f(0.90f, 0.90f, 0.90f, 0.95f);
  GPU_line_width(1.5f);
  immBegin(GPU_PRIM_LINES, 4);
  const float button_center_x = float(add_button_rect.xmin + add_button_rect.xmax) * 0.5f;
  const float button_center_y = float(add_button_rect.ymin + add_button_rect.ymax) * 0.5f;
  const float plus_half_size = std::max(4.0f, float(BLI_rcti_size_x(&add_button_rect)) * 0.22f);
  immVertex2f(pos, button_center_x - plus_half_size, button_center_y);
  immVertex2f(pos, button_center_x + plus_half_size, button_center_y);
  immVertex2f(pos, button_center_x, button_center_y - plus_half_size);
  immVertex2f(pos, button_center_x, button_center_y + plus_half_size);
  immEnd();

  immUniformColor4f(0.29f, 0.58f, 0.96f, 0.9f);
  immRectf(pos,
           float(left_panel_width),
           float(content_top - 2),
           float(region->winx),
           float(content_top));

  if (better_timeline_track_scrollbar_visible(region, sbetter_timeline)) {
    const rcti scrollbar_rect = better_timeline_track_scrollbar_rect(region, sbetter_timeline);
    const rcti thumb_rect = better_timeline_track_scrollbar_thumb_rect(region, sbetter_timeline);
    bTheme *btheme = ui::theme::theme_get();
    uiWidgetColors wcol = btheme->tui.wcol_scroll;
    const char emboss_alpha = btheme->tui.widget_emboss[3];
    const bool scrollbar_active = false;

    if (wcol.inner[3] == 0) {
      wcol.inner[3] = 64;
    }
    wcol.outline[3] = 0;
    btheme->tui.widget_emboss[3] = 0;
    ui::draw_widget_scroll(
        &wcol, &scrollbar_rect, &thumb_rect, scrollbar_active ? ui::SCROLL_PRESSED : 0);
    btheme->tui.widget_emboss[3] = emboss_alpha;
  }

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);

  better_timeline_clip_begin(region, content_rect, &content_clip_state);
  for (int row_index = 0; row_index < track_count; row_index++) {
    if (!better_timeline_row_is_visible(region, sbetter_timeline, row_index)) {
      continue;
    }
    const BetterTimelineTrack *track = better_timeline_track_at_index(sbetter_timeline, row_index);
    if (track == nullptr) {
      continue;
    }
    uchar text_color[4];
    ui::theme::get_color_4ubv(better_timeline_track_is_selected(track) ? TH_HEADER_TEXT_HI :
                                                                           TH_TEXT,
                              text_color);
    BLF_color4ubv(BLF_default(), text_color);
    const float y = better_timeline_row_ymin(region, sbetter_timeline, row_index) +
                    (BETTER_TIMELINE_ROW_HEIGHT * 0.5f) - (5.0f * UI_SCALE_FAC);
    BLF_draw_default(16.0f * UI_SCALE_FAC, y, 0.0f, track->name, BLF_DRAW_STR_DUMMY_MAX);
  }
  better_timeline_clip_end(content_clip_state);

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

  const rcti body_rect = better_timeline_body_rect(region, sbetter_timeline);
  const float current_frame_x = ui::view2d_view_to_region_x(v2d, BKE_scene_ctime_get(scene));
  BetterTimelineClipState clip_state;
  better_timeline_clip_begin(region, body_rect, &clip_state);
  better_timeline_draw_grid_x_frames(v2d, scene);
  better_timeline_view_ortho(v2d);
  ANIM_draw_framerange(scene, v2d);
  if (current_frame_x >= body_rect.xmin && current_frame_x <= body_rect.xmax) {
    ANIM_draw_cfra(C, v2d, DRAWCFRA_WIDE);
  }
  ui::view2d_view_restore(C);
  better_timeline_clip_end(clip_state);

  const rcti scrub_rect = better_timeline_scrub_rect(region, sbetter_timeline);
  better_timeline_clip_begin(region, scrub_rect, &clip_state);
  ED_time_scrub_draw(region, scene, false, true, round_db_to_int(scene->frames_per_second()));
  better_timeline_clip_end(clip_state);
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
  const rcti scrub_rect = better_timeline_scrub_rect(region, sbetter_timeline);
  const float current_frame_x = ui::view2d_view_to_region_x(&region->v2d, BKE_scene_ctime_get(scene));
  BetterTimelineClipState clip_state;
  if (current_frame_x >= scrub_rect.xmin && current_frame_x <= scrub_rect.xmax) {
    better_timeline_clip_begin(region, scrub_rect, &clip_state);
    ED_time_scrub_draw_current_frame(region, scene, false, false);
    better_timeline_clip_end(clip_state);
  }
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

static void better_timeline_space_blend_read_data(BlendDataReader *reader, SpaceLink *sl)
{
  auto *sbetter_timeline = reinterpret_cast<SpaceBetterTimeline *>(sl);

  BLO_read_struct_list(reader, BetterTimelineTrack, &sbetter_timeline->tracks);

  if (!better_timeline_has_selected_track(sbetter_timeline)) {
    sbetter_timeline->selected_track_index = -1;
  }
  else if (const BetterTimelineTrack *active_track = better_timeline_track_at_index(
               sbetter_timeline, sbetter_timeline->selected_track_index))
  {
    if (!better_timeline_track_is_selected(active_track)) {
      sbetter_timeline->selected_track_index = better_timeline_first_selected_track_index(
          sbetter_timeline);
    }
  }
  else {
    sbetter_timeline->selected_track_index = better_timeline_first_selected_track_index(
        sbetter_timeline);
  }
  sbetter_timeline->next_track_name_index = std::max(1, sbetter_timeline->next_track_name_index);
}

static void better_timeline_space_blend_write(BlendWriter *writer, SpaceLink *sl)
{
  auto *sbetter_timeline = reinterpret_cast<SpaceBetterTimeline *>(sl);

  writer->write_struct_list(&sbetter_timeline->tracks);
  writer->write_struct(sbetter_timeline);
}

void ED_better_timeline_undosys_type(UndoType *ut)
{
  ut->name = "Better Timeline";
  ut->poll = better_timeline_undosys_poll;
  ut->step_encode = better_timeline_undosys_step_encode;
  ut->step_decode = better_timeline_undosys_step_decode;
  ut->step_free = better_timeline_undosys_step_free;
  ut->flags = UNDOTYPE_FLAG_NEED_CONTEXT_FOR_ENCODE;
  ut->step_size = sizeof(BetterTimelineUndoStep);
}

void ED_spacetype_better_timeline()
{
  std::unique_ptr<SpaceType> st = std::make_unique<SpaceType>();
  ARegionType *art;

  WM_operatortype_append(BETTER_TIMELINE_OT_add_track);
  WM_operatortype_append(BETTER_TIMELINE_OT_add_track_menu);
  WM_operatortype_append(BETTER_TIMELINE_OT_delete_track);
  WM_operatortype_append(BETTER_TIMELINE_OT_clear_selection);
  WM_operatortype_append(BETTER_TIMELINE_OT_scrollbar_drag);
  WM_operatortype_append(BETTER_TIMELINE_OT_scroll_tracks);
  WM_operatortype_append(BETTER_TIMELINE_OT_track_select);
  WM_operatortype_append(BETTER_TIMELINE_OT_track_reorder);
  WM_operatortype_append(BETTER_TIMELINE_OT_resize_panel);
  WM_operatortype_append(BETTER_TIMELINE_OT_view_all);

  st->spaceid = SPACE_BETTER_TIMELINE;
  STRNCPY_UTF8(st->name, "Better Timeline");

  st->create = better_timeline_create;
  st->free = better_timeline_free;
  st->init = better_timeline_init;
  st->duplicate = better_timeline_duplicate;
  st->blend_read_data = better_timeline_space_blend_read_data;
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
