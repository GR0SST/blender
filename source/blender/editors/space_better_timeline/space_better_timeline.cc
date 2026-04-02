/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#include <cstring>
#include <memory>

#include "DNA_space_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "better_timeline_intern.hh" /* own include */

namespace blender {

bool better_timeline_operator_region_poll(bContext *C)
{
  const ScrArea *area = CTX_wm_area(C);
  const ARegion *region = CTX_wm_region(C);
  return area != nullptr && region != nullptr && area->spacetype == SPACE_BETTER_TIMELINE &&
         region->regiontype == RGN_TYPE_WINDOW;
}

static SpaceLink *better_timeline_create(const ScrArea * /*area*/, const Scene * /*scene*/)
{
  ARegion *region;
  SpaceBetterTimeline *sbetter_timeline;

  sbetter_timeline = MEM_new<SpaceBetterTimeline>("init better timeline");
  sbetter_timeline->spacetype = SPACE_BETTER_TIMELINE;
  better_timeline_space_state_init(sbetter_timeline);

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
  BLI_listbase_clear(&sbetter_timeline->tracks);
  better_timeline_tracks_duplicate(&sbetter_timeline->tracks,
                                   &reinterpret_cast<SpaceBetterTimeline *>(sl)->tracks);
  return reinterpret_cast<SpaceLink *>(sbetter_timeline);
}

static void better_timeline_main_region_init(wmWindowManager *wm, ARegion *region)
{
  ui::view2d_region_reinit(&region->v2d, ui::V2D_COMMONVIEW_CUSTOM, region->winx, region->winy);
  better_timeline_main_region_keymap_init(wm, region);
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

void ED_spacetype_better_timeline()
{
  std::unique_ptr<SpaceType> st = std::make_unique<SpaceType>();
  ARegionType *art;

  better_timeline_track_ops_register();
  better_timeline_view_ops_register();

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
