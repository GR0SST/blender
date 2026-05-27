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
#include "BLI_vector.hh"

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
  const int max_width = std::clamp(region->winx -
                                       int(BETTER_TIMELINE_TIMELINE_MIN_WIDTH * UI_SCALE_FAC),
                                   min_width,
                                   hard_max_width);
  return std::clamp(panel_width, min_width, max_width);
}

static int better_timeline_default_left_panel_width(const ARegion *region)
{
  return better_timeline_panel_width_clamp(
      region, int(BETTER_TIMELINE_PANEL_DEFAULT_WIDTH * UI_SCALE_FAC));
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

rcti better_timeline_body_rect(const ARegion *region, const SpaceBetterTimeline *sbetter_timeline)
{
  rcti rect{};
  rect.xmin = better_timeline_left_panel_width(region, sbetter_timeline);
  rect.xmax = std::max(rect.xmin + 1, int(region->winx));
  rect.ymin = 0;
  rect.ymax = std::max(1, better_timeline_content_height(region));
  return rect;
}

rcti better_timeline_scrub_rect(const ARegion *region, const SpaceBetterTimeline *sbetter_timeline)
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

int better_timeline_row_height()
{
  return int(BETTER_TIMELINE_ROW_HEIGHT * UI_SCALE_FAC);
}

static int better_timeline_row_count(const ARegion *region)
{
  const int content_height = better_timeline_content_height(region);
  return std::max(BETTER_TIMELINE_MIN_ROWS, content_height / better_timeline_row_height());
}

int better_timeline_track_scroll_max(const ARegion *region,
                                     const SpaceBetterTimeline *sbetter_timeline)
{
  const int total_track_height = better_timeline_visible_row_count(sbetter_timeline) *
                                 better_timeline_row_height();
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
                                          better_timeline_visible_row_count(sbetter_timeline) *
                                              better_timeline_row_height());
  const int scroll_max = better_timeline_track_scroll_max(region, sbetter_timeline);
  const int thumb_height = std::clamp(
      int(std::round(float(visible_height) * float(scrollbar_height) / float(total_track_height))),
      int(BETTER_TIMELINE_SCROLLBAR_MIN_THUMB_HEIGHT * UI_SCALE_FAC),
      scrollbar_height);
  const int travel = std::max(0, scrollbar_height - thumb_height);
  const float scroll_ratio = (scroll_max > 0) ? float(better_timeline_track_scroll_offset(
                                                    region, sbetter_timeline)) /
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
  return float(content_top + scroll_offset - (row_index * better_timeline_row_height()));
}

float better_timeline_row_ymin(const ARegion *region,
                               const SpaceBetterTimeline *sbetter_timeline,
                               const int row_index)
{
  return better_timeline_row_ymax(region, sbetter_timeline, row_index) -
         better_timeline_row_height();
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
  /* Returns a *visible row index* (not a flat track index). */
  const int content_top = better_timeline_content_height(region);
  const int scroll_offset = better_timeline_track_scroll_offset(region, sbetter_timeline);
  const int row_count = better_timeline_visible_row_count(sbetter_timeline);

  if (region_y >= content_top) {
    return -1;
  }
  if (region_y < 0) {
    return -1;
  }

  const int row_index = (content_top + scroll_offset - region_y - 1) /
                        better_timeline_row_height();
  return (row_index >= 0 && row_index < row_count) ? row_index : -1;
}

int better_timeline_track_insertion_index_from_region_y(
    const ARegion *region, const SpaceBetterTimeline *sbetter_timeline, const int region_y)
{
  /* Returns a flat top-level insertion index for reorder-drag purposes.
   * Maps visible-row Y coordinates back to a flat track list position. */
  const int flat_count = better_timeline_track_count(sbetter_timeline);
  if (flat_count == 0) {
    return 0;
  }

  const int content_top = better_timeline_content_height(region);
  if (region_y >= content_top) {
    return 0;
  }
  if (region_y < 0) {
    return flat_count;
  }

  /* Use visible row to get a Y position, then walk visible rows to find which
   * top-level track the hover corresponds to, yielding a flat insertion index. */
  const int visible_row = better_timeline_track_from_region_y(region, sbetter_timeline, region_y);
  if (visible_row < 0) {
    return flat_count;
  }

  /* Find the top-level track whose visible row span contains this row. */
  const Vector<BetterTimelineVisibleRow> rows = better_timeline_visible_rows_build(
      sbetter_timeline);
  if (visible_row >= int(rows.size())) {
    return flat_count;
  }

  /* Walk up to find the top-level parent (indent == 0). */
  int top_level_row = visible_row;
  while (top_level_row > 0 && rows[top_level_row].indent > 0) {
    top_level_row--;
  }

  /* Map the top-level visible row to a flat track index. */
  int flat_index = 0;
  for (BetterTimelineTrack *track =
           static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next, flat_index++)
  {
    if (rows[top_level_row].track == track) {
      /* Use the Y centre of the hover row to decide insert before or after. */
      const float row_y_center = (better_timeline_row_ymin(region, sbetter_timeline, visible_row) +
                                  better_timeline_row_ymax(
                                      region, sbetter_timeline, visible_row)) *
                                 0.5f;
      return (float(region_y) >= row_y_center) ? flat_index : flat_index + 1;
    }
  }
  return flat_count;
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
  for (const BetterTimelineTrack *track =
           static_cast<const BetterTimelineTrack *>(sbetter_timeline->tracks.first);
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

    const int current_index = better_timeline_track_index_from_ptr(sbetter_timeline,
                                                                   selected_track);
    if (current_index < 0) {
      return false;
    }

    const int target_index = std::clamp((insertion_index <= current_index) ? insertion_index :
                                                                             insertion_index - 1,
                                        0,
                                        std::max(0, track_count - 1));
    return better_timeline_reorder_track_to_index(sbetter_timeline, selected_track, target_index);
  }

  int selected_before_insertion = 0;
  int index = 0;
  for (const BetterTimelineTrack *track =
           static_cast<const BetterTimelineTrack *>(sbetter_timeline->tracks.first);
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

BetterTimelineInsertionTarget better_timeline_insertion_target_from_region_y(
    const ARegion *region, const SpaceBetterTimeline *sbetter_timeline, const int region_y)
{
  BetterTimelineInsertionTarget result;
  const Vector<BetterTimelineVisibleRow> rows = better_timeline_visible_rows_build(
      sbetter_timeline);
  const int row_count = int(rows.size());

  if (row_count == 0) {
    return result;
  }

  const int content_top = better_timeline_content_height(region);
  int above_row_idx = -1;
  int below_row_idx = 0;

  if (region_y >= content_top) {
    above_row_idx = -1;
    below_row_idx = 0;
  }
  else if (region_y < 0) {
    above_row_idx = row_count - 1;
    below_row_idx = row_count;
  }
  else {
    const int hovered = better_timeline_track_from_region_y(region, sbetter_timeline, region_y);
    if (hovered < 0) {
      above_row_idx = row_count - 1;
      below_row_idx = row_count;
    }
    else {
      const float row_ymin = float(better_timeline_row_ymin(region, sbetter_timeline, hovered));
      const float row_ymax = float(better_timeline_row_ymax(region, sbetter_timeline, hovered));
      const bool insert_before = (float(region_y) >= (row_ymin + row_ymax) * 0.5f);
      if (insert_before) {
        above_row_idx = hovered - 1;
        below_row_idx = hovered;
      }
      else {
        above_row_idx = hovered;
        below_row_idx = hovered + 1;
      }
    }
  }

  const BetterTimelineVisibleRow *above_row = (above_row_idx >= 0 && above_row_idx < row_count) ?
                                                  &rows[above_row_idx] :
                                                  nullptr;
  const BetterTimelineVisibleRow *below_row = (below_row_idx >= 0 && below_row_idx < row_count) ?
                                                  &rows[below_row_idx] :
                                                  nullptr;

  /* Case 1: both adjacent rows are children of the same group → insert inside that group. */
  if (above_row != nullptr && below_row != nullptr && above_row->parent_group != nullptr &&
      above_row->parent_group == below_row->parent_group)
  {
    BetterTimelineTrack *group = above_row->parent_group;
    /* Cycle guard: if any selected track is an ancestor of this group, fall through
     * to top-level insertion instead. */
    if (!better_timeline_would_create_group_cycle(sbetter_timeline, group)) {
      int idx = 0;
      for (const BetterTimelineTrack *t =
               static_cast<const BetterTimelineTrack *>(group->group_tracks.first);
           t != nullptr;
           t = t->next, idx++)
      {
        if (t == above_row->track) {
          result.parent_group = group;
          result.index = idx + 1;
          return result;
        }
      }
      result.parent_group = group;
      result.index = BLI_listbase_count(&group->group_tracks);
      return result;
    }
  }

  /* Case 2: below_row is the first child of a group (cursor just below group header). */
  if (below_row != nullptr && below_row->parent_group != nullptr &&
      (above_row == nullptr || above_row->track == below_row->parent_group))
  {
    BetterTimelineTrack *group = below_row->parent_group;
    if (!better_timeline_would_create_group_cycle(sbetter_timeline, group)) {
      result.parent_group = group;
      result.index = 0;
      return result;
    }
  }

  /* Default: top-level insertion. */
  if (above_row == nullptr) {
    result.parent_group = nullptr;
    result.index = 0;
    return result;
  }

  /* Walk up to find the top-level track covering above_row. */
  int top_row_idx = above_row_idx;
  while (top_row_idx > 0 && rows[top_row_idx].indent > 0) {
    top_row_idx--;
  }
  BetterTimelineTrack *top_track = rows[top_row_idx].track;

  int flat_index = 0;
  for (const BetterTimelineTrack *t =
           static_cast<const BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       t != nullptr;
       t = t->next, flat_index++)
  {
    if (t == top_track) {
      result.parent_group = nullptr;
      result.index = flat_index + 1;
      return result;
    }
  }

  result.parent_group = nullptr;
  result.index = better_timeline_track_count(sbetter_timeline);
  return result;
}

float better_timeline_insertion_target_y(const ARegion *region,
                                         const SpaceBetterTimeline *sbetter_timeline,
                                         const BetterTimelineInsertionTarget &target)
{
  const Vector<BetterTimelineVisibleRow> rows = better_timeline_visible_rows_build(
      sbetter_timeline);
  const int row_count = int(rows.size());

  if (row_count == 0) {
    return float(better_timeline_content_height(region));
  }

  const ListBase *parent_list = target.parent_group ? static_cast<const ListBase *>(
                                                          &target.parent_group->group_tracks) :
                                                      &sbetter_timeline->tracks;

  /* Find the track at target.index in parent_list. */
  BetterTimelineTrack *track_at = nullptr;
  {
    int i = 0;
    for (BetterTimelineTrack *t = static_cast<BetterTimelineTrack *>(parent_list->first);
         t != nullptr;
         t = t->next, i++)
    {
      if (i == target.index) {
        track_at = t;
        break;
      }
    }
  }

  if (track_at == nullptr) {
    /* Insert at the end of parent_list. Find the last track in the list and then walk back
     * through visible rows to find the very last row that belongs to its subtree.
     * This correctly accounts for expanded group children below the last top-level entry. */
    const BetterTimelineTrack *last_in_list = nullptr;
    for (const BetterTimelineTrack *t =
             static_cast<const BetterTimelineTrack *>(parent_list->first);
         t != nullptr;
         t = t->next)
    {
      last_in_list = t;
    }

    if (last_in_list == nullptr) {
      return float(better_timeline_content_height(region));
    }

    for (int vi = row_count - 1; vi >= 0; vi--) {
      if (rows[vi].track == last_in_list ||
          better_timeline_track_is_descendant_of(rows[vi].track, last_in_list))
      {
        return better_timeline_row_ymin(region, sbetter_timeline, vi);
      }
    }
    return 0.0f;
  }

  /* Find the visible row for track_at and return its top Y edge. */
  for (int vi = 0; vi < row_count; vi++) {
    if (rows[vi].track == track_at) {
      return better_timeline_row_ymax(region, sbetter_timeline, vi);
    }
  }
  return 0.0f;
}

bool better_timeline_reorder_selected_tracks_would_change_target(
    const SpaceBetterTimeline *sbetter_timeline, const BetterTimelineInsertionTarget &target)
{
  const ListBase *target_list = target.parent_group ? static_cast<const ListBase *>(
                                                          &target.parent_group->group_tracks) :
                                                      &sbetter_timeline->tracks;

  /* Collect effective selection (skip children of selected groups). */
  const Vector<BetterTimelineVisibleRow> rows = better_timeline_visible_rows_build(
      sbetter_timeline);
  Vector<const BetterTimelineTrack *> selected;
  for (const BetterTimelineVisibleRow &row : rows) {
    if (!better_timeline_track_is_selected(row.track)) {
      continue;
    }
    if (better_timeline_track_has_selected_ancestor(sbetter_timeline, row.track)) {
      continue;
    }
    selected.append(row.track);
  }
  if (selected.is_empty()) {
    return false;
  }

  if (selected.size() == 1) {
    const BetterTimelineTrack *sel = selected[0];
    int cur_idx = 0;
    for (const BetterTimelineTrack *t =
             static_cast<const BetterTimelineTrack *>(target_list->first);
         t != nullptr;
         t = t->next, cur_idx++)
    {
      if (t == sel) {
        return !(cur_idx == target.index || cur_idx + 1 == target.index);
      }
    }
    return true; /* Not in target list yet — will be moved in. */
  }

  return true; /* Multi-selection: conservatively assume change. */
}

bool better_timeline_reorder_selected_tracks_to_target(SpaceBetterTimeline *sbetter_timeline,
                                                       const BetterTimelineInsertionTarget &target)
{
  /* Cycle guard: refuse if moving selected tracks into target.parent_group would create a cycle.
   */
  if (target.parent_group != nullptr &&
      better_timeline_would_create_group_cycle(sbetter_timeline, target.parent_group))
  {
    return false;
  }

  /* Collect selected tracks with their current owner list.
   * Skip children of selected groups (the group moves them automatically). */
  struct MoveInfo {
    BetterTimelineTrack *track;
    ListBase *source_list;
  };
  Vector<MoveInfo> to_move;
  {
    const Vector<BetterTimelineVisibleRow> rows = better_timeline_visible_rows_build(
        sbetter_timeline);
    for (const BetterTimelineVisibleRow &row : rows) {
      if (!better_timeline_track_is_selected(row.track)) {
        continue;
      }
      if (better_timeline_track_has_selected_ancestor(sbetter_timeline, row.track)) {
        continue;
      }
      ListBase *src = row.parent_group ? static_cast<ListBase *>(&row.parent_group->group_tracks) :
                                         &sbetter_timeline->tracks;
      to_move.append({row.track, src});
    }
  }
  if (to_move.is_empty()) {
    return false;
  }

  ListBase *target_list = target.parent_group ?
                              static_cast<ListBase *>(&target.parent_group->group_tracks) :
                              &sbetter_timeline->tracks;

  /* Find anchor: last non-moved track in target_list strictly before target.index.
   * Capture before any removal so pointer remains valid. */
  BetterTimelineTrack *anchor = nullptr;
  {
    /* Which tracks are being moved out of target_list? */
    Vector<const BetterTimelineTrack *> moving_from_target;
    for (const MoveInfo &info : to_move) {
      if (info.source_list == target_list) {
        moving_from_target.append(info.track);
      }
    }

    int i = 0;
    for (BetterTimelineTrack *t = static_cast<BetterTimelineTrack *>(target_list->first);
         t != nullptr;
         t = t->next, i++)
    {
      if (i >= target.index) {
        break;
      }
      bool is_moving = false;
      for (const BetterTimelineTrack *m : moving_from_target) {
        if (m == t) {
          is_moving = true;
          break;
        }
      }
      if (!is_moving) {
        anchor = t;
      }
    }
  }

  /* Remove selected tracks from their respective source lists. */
  for (const MoveInfo &info : to_move) {
    BLI_remlink(info.source_list, info.track);
  }

  /* Insert after anchor in target_list, preserving relative order. */
  BetterTimelineTrack *insert_after = anchor;
  for (const MoveInfo &info : to_move) {
    BLI_insertlinkafter(target_list, insert_after, info.track);
    insert_after = info.track;
  }

  return true;
}

bool better_timeline_move_selected_tracks_into_group(SpaceBetterTimeline *sbetter_timeline,
                                                     BetterTimelineTrack *group)
{
  if (sbetter_timeline == nullptr || group == nullptr) {
    return false;
  }
  /* Cycle guard. */
  if (better_timeline_would_create_group_cycle(sbetter_timeline, group)) {
    return false;
  }
  /* Collect all selected tracks from any depth, skipping children of selected groups
   * (they move with their parent) and the target group itself. */
  struct MoveInfo {
    BetterTimelineTrack *track;
    ListBase *source_list;
  };
  Vector<MoveInfo> to_move;
  {
    const Vector<BetterTimelineVisibleRow> rows = better_timeline_visible_rows_build(
        sbetter_timeline);
    for (const BetterTimelineVisibleRow &row : rows) {
      if (!better_timeline_track_is_selected(row.track)) {
        continue;
      }
      if (row.track == group) {
        continue;
      }
      if (better_timeline_track_has_selected_ancestor(sbetter_timeline, row.track)) {
        continue; /* Parent group moves this child along. */
      }
      ListBase *src = row.parent_group ? static_cast<ListBase *>(&row.parent_group->group_tracks) :
                                         &sbetter_timeline->tracks;
      to_move.append({row.track, src});
    }
  }

  for (const MoveInfo &info : to_move) {
    BLI_remlink(info.source_list, info.track);
    BLI_addtail(&group->group_tracks, info.track);
  }
  return !to_move.is_empty();
}

static int better_timeline_track_reorder_autoscroll_step(
    const ARegion *region, const SpaceBetterTimeline *sbetter_timeline, const int region_y)
{
  const int scroll_max = better_timeline_track_scroll_max(region, sbetter_timeline);
  if (scroll_max == 0) {
    return 0;
  }

  const int content_top = better_timeline_content_height(region);
  const int edge_size = std::max(12, int(24.0f * UI_SCALE_FAC));
  const int scroll_step = std::max(1, better_timeline_row_height() / 4);

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
  /* insertion_index here is a flat top-level index; map it to a visible row Y.
   * Find the first visible row whose top-level track is at flat index `insertion_index`. */
  const int flat_count = better_timeline_track_count(sbetter_timeline);
  const int visible_count = better_timeline_visible_row_count(sbetter_timeline);
  const int clamped = std::clamp(insertion_index, 0, flat_count);

  if (clamped <= 0) {
    return float(better_timeline_content_height(region));
  }
  if (clamped >= flat_count) {
    return (visible_count > 0) ?
               better_timeline_row_ymin(region, sbetter_timeline, visible_count - 1) :
               0.0f;
  }

  /* Walk visible rows to find the first row that belongs to flat track at clamped index. */
  const Vector<BetterTimelineVisibleRow> rows = better_timeline_visible_rows_build(
      sbetter_timeline);
  int flat_index = 0;
  for (BetterTimelineTrack *track =
           static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next, flat_index++)
  {
    if (flat_index == clamped) {
      for (int vi = 0; vi < int(rows.size()); vi++) {
        if (rows[vi].track == track) {
          return better_timeline_row_ymax(region, sbetter_timeline, vi);
        }
      }
      break;
    }
  }
  return (visible_count > 0) ?
             better_timeline_row_ymin(region, sbetter_timeline, visible_count - 1) :
             0.0f;
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
  const float total_rows = float(std::max(better_timeline_row_count(region),
                                          better_timeline_visible_row_count(sbetter_timeline)));
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
  /* Indent child tracks the same way accent bar and icons are indented in draw. */
  const Vector<BetterTimelineVisibleRow> rows = better_timeline_visible_rows_build(
      sbetter_timeline);
  const int indent = (row_index >= 0 && row_index < int(rows.size())) ? rows[row_index].indent : 0;
  const int indent_x = int(float(indent) * 16.0f * UI_SCALE_FAC);
  /* Start after indent + accent + type icon; end before the mute button. */
  const int x_min = indent_x + accent_w + gap + icon_area + gap;
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
  const int row_count = better_timeline_visible_row_count(sbetter_timeline);
  for (int i = 0; i < row_count; i++) {
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
  const int row_count = better_timeline_visible_row_count(sbetter_timeline);
  for (int i = 0; i < row_count; i++) {
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

rcti better_timeline_group_collapse_toggle_rect(const ARegion *region,
                                                const SpaceBetterTimeline *sbetter_timeline,
                                                const int row_index)
{
  const float y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
  const int row_h = better_timeline_row_height();
  const int btn_size = int(BETTER_TIMELINE_TRACK_BUTTON_SIZE * UI_SCALE_FAC);
  const int accent_w = int(BETTER_TIMELINE_TRACK_ACCENT_WIDTH * UI_SCALE_FAC);

  /* Apply row indent so the hit zone matches the visually indented arrow. */
  const Vector<BetterTimelineVisibleRow> rows = better_timeline_visible_rows_build(
      sbetter_timeline);
  const int indent = (row_index >= 0 && row_index < int(rows.size())) ? rows[row_index].indent : 0;
  const int indent_x = int(float(indent) * 16.0f * UI_SCALE_FAC);

  rcti rect{};
  rect.xmin = indent_x + accent_w;
  rect.xmax = indent_x + accent_w + btn_size;
  rect.ymin = int(y_min) + std::max(0, (row_h - btn_size) / 2);
  rect.ymax = rect.ymin + btn_size;
  return rect;
}

bool better_timeline_is_on_group_collapse_toggle(const ARegion *region,
                                                 const SpaceBetterTimeline *sbetter_timeline,
                                                 const int region_x,
                                                 const int region_y,
                                                 int *r_group_row_index)
{
  if (r_group_row_index != nullptr) {
    *r_group_row_index = -1;
  }
  const int row_count = better_timeline_visible_row_count(sbetter_timeline);
  const Vector<BetterTimelineVisibleRow> rows = better_timeline_visible_rows_build(
      sbetter_timeline);
  for (int i = 0; i < row_count; i++) {
    if (!better_timeline_row_is_visible(region, sbetter_timeline, i)) {
      continue;
    }
    if (!better_timeline_track_is_group(rows[i].track)) {
      continue;
    }
    const rcti rect = better_timeline_group_collapse_toggle_rect(region, sbetter_timeline, i);
    if (BLI_rcti_isect_pt(&rect, region_x, region_y)) {
      if (r_group_row_index != nullptr) {
        *r_group_row_index = i;
      }
      return true;
    }
  }
  return false;
}

}  // namespace blender
