/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#include <algorithm>
#include <cmath>
#include <vector>

#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_utildefines.h"

#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"

#include "better_timeline_intern.hh" /* own include */

namespace blender {

int better_timeline_panel_width_clamp(const ARegion *region, const int panel_width)
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
  return better_timeline_panel_width_clamp(region,
                                           int(BETTER_TIMELINE_PANEL_DEFAULT_WIDTH * UI_SCALE_FAC));
}

int better_timeline_left_panel_width(const ARegion *region,
                                     const SpaceBetterTimeline *sbetter_timeline)
{
  const int stored_width = (sbetter_timeline != nullptr) ? sbetter_timeline->track_panel_width : 0;
  const int panel_width = (stored_width > 0) ? stored_width :
                                             better_timeline_default_left_panel_width(region);
  return better_timeline_panel_width_clamp(region, panel_width);
}

bool better_timeline_is_on_panel_divider(const ARegion *region,
                                         const SpaceBetterTimeline *sbetter_timeline,
                                         const int region_x)
{
  const int divider_x = better_timeline_left_panel_width(region, sbetter_timeline);
  return std::abs(region_x - divider_x) <= BETTER_TIMELINE_DIVIDER_HIT_WIDTH;
}

void better_timeline_view2d_update_old_window(ARegion *region,
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

rcti better_timeline_body_rect(const ARegion *region,
                               const SpaceBetterTimeline *sbetter_timeline)
{
  rcti rect{};
  rect.xmin = better_timeline_left_panel_width(region, sbetter_timeline);
  rect.xmax = std::max(rect.xmin + 1, int(region->winx));
  rect.ymin = 0;
  rect.ymax = std::max(1, better_timeline_content_height(region));
  return rect;
}

rcti better_timeline_scrub_rect(const ARegion *region,
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

rcti better_timeline_add_button_rect(const ARegion *region,
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

bool better_timeline_is_in_add_button(const ARegion *region,
                                      const SpaceBetterTimeline *sbetter_timeline,
                                      const int region_x,
                                      const int region_y)
{
  const rcti rect = better_timeline_add_button_rect(region, sbetter_timeline);
  return BLI_rcti_isect_pt(&rect, region_x, region_y);
}

bool better_timeline_scrub_event_in_region(const ScrArea *area,
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

int better_timeline_content_height(const ARegion *region)
{
  return std::max(0, region->winy - int(UI_TIME_SCRUB_MARGIN_Y));
}

static int better_timeline_row_count(const ARegion *region)
{
  const int content_height = better_timeline_content_height(region);
  return std::max(BETTER_TIMELINE_MIN_ROWS, content_height / BETTER_TIMELINE_ROW_HEIGHT);
}

int better_timeline_track_scroll_max(const ARegion *region,
                                     const SpaceBetterTimeline *sbetter_timeline)
{
  const int total_track_height = better_timeline_track_count(sbetter_timeline) *
                                 BETTER_TIMELINE_ROW_HEIGHT;
  return std::max(0, total_track_height - better_timeline_content_height(region));
}

int better_timeline_track_scroll_offset(const ARegion *region,
                                        const SpaceBetterTimeline *sbetter_timeline)
{
  return std::clamp((sbetter_timeline != nullptr) ? sbetter_timeline->track_scroll_offset : 0,
                    0,
                    better_timeline_track_scroll_max(region, sbetter_timeline));
}

bool better_timeline_track_scrollbar_visible(const ARegion *region,
                                             const SpaceBetterTimeline *sbetter_timeline)
{
  return better_timeline_track_scroll_max(region, sbetter_timeline) > 0;
}

rcti better_timeline_track_scrollbar_rect(const ARegion *region,
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

rcti better_timeline_track_scrollbar_thumb_rect(const ARegion *region,
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

bool better_timeline_is_in_track_scrollbar(const ARegion *region,
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

bool better_timeline_is_in_track_list_pane(const ARegion *region,
                                           const SpaceBetterTimeline *sbetter_timeline,
                                           const int region_x)
{
  return region_x < better_timeline_left_panel_width(region, sbetter_timeline);
}

float better_timeline_row_ymax(const ARegion *region,
                               const SpaceBetterTimeline *sbetter_timeline,
                               const int row_index)
{
  const int content_top = better_timeline_content_height(region);
  const int scroll_offset = better_timeline_track_scroll_offset(region, sbetter_timeline);
  return float(content_top + scroll_offset - (row_index * BETTER_TIMELINE_ROW_HEIGHT));
}

float better_timeline_row_ymin(const ARegion *region,
                               const SpaceBetterTimeline *sbetter_timeline,
                               const int row_index)
{
  return better_timeline_row_ymax(region, sbetter_timeline, row_index) -
         BETTER_TIMELINE_ROW_HEIGHT;
}

bool better_timeline_row_is_visible(const ARegion *region,
                                    const SpaceBetterTimeline *sbetter_timeline,
                                    const int row_index)
{
  const float y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
  const float y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
  const int content_top = better_timeline_content_height(region);
  return y_max > 0.0f && y_min < content_top;
}

int better_timeline_track_from_region_y(const ARegion *region,
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

  const int track_index = (content_top + scroll_offset - region_y - 1) /
                          BETTER_TIMELINE_ROW_HEIGHT;
  return (track_index >= 0 && track_index < track_count) ? track_index : -1;
}

int better_timeline_track_insertion_index_from_region_y(const ARegion *region,
                                                        const SpaceBetterTimeline *sbetter_timeline,
                                                        const int region_y)
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

  BetterTimelineTrack *insert_after = better_timeline_track_at_index(sbetter_timeline, target_index);
  if (insert_after == nullptr || insert_after == track) {
    return false;
  }
  BLI_remlink(&sbetter_timeline->tracks, track);
  BLI_insertlinkafter(&sbetter_timeline->tracks, insert_after, track);
  return true;
}

bool better_timeline_reorder_selected_tracks_would_change(
    const SpaceBetterTimeline *sbetter_timeline, const int insertion_index)
{
  const int track_count = better_timeline_track_count(sbetter_timeline);
  const int selected_track_count = better_timeline_selected_track_count(sbetter_timeline);
  if (sbetter_timeline == nullptr || track_count < 2 || selected_track_count < 1) {
    return false;
  }

  std::vector<const BetterTimelineTrack *> original_order;
  std::vector<const BetterTimelineTrack *> reordered;
  std::vector<const BetterTimelineTrack *> selected_tracks;
  std::vector<const BetterTimelineTrack *> unselected_tracks;
  original_order.reserve(track_count);
  reordered.reserve(track_count);
  selected_tracks.reserve(selected_track_count);
  unselected_tracks.reserve(track_count - selected_track_count);

  int selected_before_insertion = 0;
  int index = 0;
  for (const BetterTimelineTrack *track = static_cast<const BetterTimelineTrack *>(
           sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next, index++)
  {
    original_order.push_back(track);
    if (index < insertion_index && better_timeline_track_is_selected(track)) {
      selected_before_insertion++;
    }
    if (better_timeline_track_is_selected(track)) {
      selected_tracks.push_back(track);
    }
    else {
      unselected_tracks.push_back(track);
    }
  }

  if (selected_tracks.empty()) {
    return false;
  }

  const int adjusted_insertion_index = std::clamp(
      insertion_index - selected_before_insertion, 0, int(unselected_tracks.size()));

  reordered.insert(reordered.end(),
                   unselected_tracks.begin(),
                   unselected_tracks.begin() + adjusted_insertion_index);
  reordered.insert(reordered.end(), selected_tracks.begin(), selected_tracks.end());
  reordered.insert(reordered.end(),
                   unselected_tracks.begin() + adjusted_insertion_index,
                   unselected_tracks.end());

  return reordered != original_order;
}

bool better_timeline_reorder_selected_tracks_to_insertion_index(
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

  BetterTimelineTrack *insert_before = better_timeline_track_at_index(sbetter_timeline,
                                                                      adjusted_insertion_index);
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

bool better_timeline_track_reorder_autoscroll_apply(const ARegion *region,
                                                    SpaceBetterTimeline *sbetter_timeline,
                                                    const int region_y)
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

float better_timeline_track_insertion_y(const ARegion *region,
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

void better_timeline_view_sync(ARegion *region,
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

rcti better_timeline_track_object_slot_rect(const ARegion *region,
                                             const SpaceBetterTimeline *sbetter_timeline,
                                             const int row_index)
{
  const int accent_w = int(BETTER_TIMELINE_TRACK_ACCENT_WIDTH * UI_SCALE_FAC);
  const int icon_area = int(BETTER_TIMELINE_TRACK_BUTTON_SIZE * UI_SCALE_FAC);
  const int gap = int(4.0f * UI_SCALE_FAC);
  const int btn_size = int(BETTER_TIMELINE_TRACK_BUTTON_SIZE * UI_SCALE_FAC);
  const int btn_margin = int(4.0f * UI_SCALE_FAC);
  const int left_panel_width = better_timeline_left_panel_width(region, sbetter_timeline);
  const float y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
  const float y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
  const int pad_y = int(5.0f * UI_SCALE_FAC);
  /* Start after accent + type icon; end before the mute button. */
  const int x_min = accent_w + gap + icon_area + gap;
  const int x_max = left_panel_width - btn_margin - 2 * btn_size - gap;
  return rcti{x_min, std::max(x_min + 1, x_max), int(y_min) + pad_y, int(y_max) - pad_y};
}

rcti better_timeline_track_object_slot_picker_rect(const ARegion *region,
                                                   const SpaceBetterTimeline *sbetter_timeline,
                                                   const int row_index)
{
  const rcti slot = better_timeline_track_object_slot_rect(region, sbetter_timeline, row_index);
  const int h = slot.ymax - slot.ymin;
  return rcti{slot.xmax - h, slot.xmax, slot.ymin, slot.ymax};
}

rcti better_timeline_track_mute_button_rect(const ARegion *region,
                                            const SpaceBetterTimeline *sbetter_timeline,
                                            const int row_index)
{
  const int left_panel_width = better_timeline_left_panel_width(region, sbetter_timeline);
  const int btn_size = int(BETTER_TIMELINE_TRACK_BUTTON_SIZE * UI_SCALE_FAC);
  const int btn_margin = int(4.0f * UI_SCALE_FAC);
  const float y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
  const float y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
  const int row_center_y = int((y_min + y_max) * 0.5f);
  const int btn_x_max = left_panel_width - btn_margin - btn_size;
  const int btn_x_min = btn_x_max - btn_size;
  return rcti{btn_x_min, btn_x_max, row_center_y - btn_size / 2, row_center_y + btn_size / 2};
}

rcti better_timeline_track_lock_button_rect(const ARegion *region,
                                            const SpaceBetterTimeline *sbetter_timeline,
                                            const int row_index)
{
  const int left_panel_width = better_timeline_left_panel_width(region, sbetter_timeline);
  const int btn_size = int(BETTER_TIMELINE_TRACK_BUTTON_SIZE * UI_SCALE_FAC);
  const int btn_margin = int(4.0f * UI_SCALE_FAC);
  const float y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
  const float y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
  const int row_center_y = int((y_min + y_max) * 0.5f);
  const int btn_x_max = left_panel_width - btn_margin;
  const int btn_x_min = btn_x_max - btn_size;
  return rcti{btn_x_min, btn_x_max, row_center_y - btn_size / 2, row_center_y + btn_size / 2};
}

int better_timeline_track_from_mute_button_region_pos(const ARegion *region,
                                                      const SpaceBetterTimeline *sbetter_timeline,
                                                      const int region_x,
                                                      const int region_y)
{
  const int track_count = better_timeline_track_count(sbetter_timeline);
  for (int i = 0; i < track_count; i++) {
    if (!better_timeline_row_is_visible(region, sbetter_timeline, i)) {
      continue;
    }
    const rcti btn_rect = better_timeline_track_mute_button_rect(region, sbetter_timeline, i);
    if (BLI_rcti_isect_pt(&btn_rect, region_x, region_y)) {
      return i;
    }
  }
  return -1;
}

int better_timeline_track_from_lock_button_region_pos(const ARegion *region,
                                                      const SpaceBetterTimeline *sbetter_timeline,
                                                      const int region_x,
                                                      const int region_y)
{
  const int track_count = better_timeline_track_count(sbetter_timeline);
  for (int i = 0; i < track_count; i++) {
    if (!better_timeline_row_is_visible(region, sbetter_timeline, i)) {
      continue;
    }
    const rcti btn_rect = better_timeline_track_lock_button_rect(region, sbetter_timeline, i);
    if (BLI_rcti_isect_pt(&btn_rect, region_x, region_y)) {
      return i;
    }
  }
  return -1;
}

}  // namespace blender
