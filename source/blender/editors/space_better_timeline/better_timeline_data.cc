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
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_undo_system.hh"

#include "BLO_read_write.hh"

#include "ED_better_timeline.hh"

#include "WM_api.hh"

#include "better_timeline_intern.hh" /* own include */

namespace blender {

static void better_timeline_clips_free(ListBase *clips)
{
  if (clips == nullptr) {
    return;
  }

  BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(clips->first);
  while (clip != nullptr) {
    BetterTimelineClip *clip_next = clip->next;
    if (clip->properties != nullptr) {
      IDP_FreeProperty(clip->properties);
      clip->properties = nullptr;
    }
    MEM_delete(clip);
    clip = clip_next;
  }

  BLI_listbase_clear(clips);
}

static BetterTimelineClip *better_timeline_clip_duplicate(const BetterTimelineClip *clip_src)
{
  if (clip_src == nullptr) {
    return nullptr;
  }

  auto *clip_dst = MEM_new<BetterTimelineClip>(__func__);
  *clip_dst = *clip_src;
  clip_dst->next = nullptr;
  clip_dst->prev = nullptr;
  clip_dst->properties = (clip_src->properties != nullptr) ? IDP_CopyProperty(clip_src->properties) :
                                                              nullptr;
  return clip_dst;
}

static void better_timeline_clips_duplicate(ListBase *dst, const ListBase *src)
{
  BLI_listbase_clear(dst);
  if (src == nullptr) {
    return;
  }

  for (const BetterTimelineClip *clip_src = static_cast<const BetterTimelineClip *>(src->first);
       clip_src != nullptr;
       clip_src = clip_src->next)
  {
    if (BetterTimelineClip *clip_dst = better_timeline_clip_duplicate(clip_src)) {
      BLI_addtail(dst, clip_dst);
    }
  }
}

static const ed::better_timeline::BetterTimelineTrackType *better_timeline_default_track_type()
{
  return ed::better_timeline::default_track_type_get();
}

static const ed::better_timeline::BetterTimelineTrackType *better_timeline_track_type_get(
    const BetterTimelineTrack *track)
{
  if (track == nullptr || track->track_type[0] == '\0') {
    return better_timeline_default_track_type();
  }
  return ed::better_timeline::track_type_find_from_idname(track->track_type);
}

static void better_timeline_track_selection_clear(ListBase *tracks)
{
  if (tracks == nullptr) {
    return;
  }

  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(tracks->first); track != nullptr;
       track = track->next)
  {
    better_timeline_track_set_selected(track, false);
  }
}

static void better_timeline_state_snapshot_from_space(BetterTimelineUndoStep *us,
                                                      const SpaceBetterTimeline *sbetter_timeline)
{
  BLI_assert(us != nullptr);
  BLI_assert(sbetter_timeline != nullptr);

  better_timeline_tracks_duplicate(&us->tracks, &sbetter_timeline->tracks);
  better_timeline_track_selection_clear(&us->tracks);
  us->selected_track_index = -1;
  us->next_track_name_index = sbetter_timeline->next_track_name_index;
  us->track_panel_width = sbetter_timeline->track_panel_width;
  us->track_scroll_offset = sbetter_timeline->track_scroll_offset;
}

static void better_timeline_state_restore(SpaceBetterTimeline *dst,
                                          const ListBase *tracks_src,
                                          int selected_track_index,
                                          int next_track_name_index,
                                          int track_panel_width,
                                          int track_scroll_offset)
{
  if (dst == nullptr) {
    return;
  }

  better_timeline_tracks_free(&dst->tracks);
  better_timeline_tracks_duplicate(&dst->tracks, tracks_src);
  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(dst->tracks.first); track != nullptr;
       track = track->next)
  {
    better_timeline_track_ensure_type(track);
    for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first);
         clip != nullptr;
         clip = clip->next)
    {
      better_timeline_clip_ensure_type(track, clip);
    }
  }
  dst->selected_track_index = selected_track_index;
  dst->next_track_name_index = std::max(1, next_track_name_index);
  dst->track_panel_width = track_panel_width;
  dst->track_scroll_offset = std::max(0, track_scroll_offset);
}

static void better_timeline_state_normalize_after_read(SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return;
  }

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
  better_timeline_state_snapshot_from_space(us, sbetter_timeline);
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
  better_timeline_tracks_free(&us->tracks);
}

void better_timeline_space_state_init(SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return;
  }

  sbetter_timeline->selected_track_index = -1;
  sbetter_timeline->next_track_name_index = 1;
  sbetter_timeline->track_panel_width = 0;
  sbetter_timeline->track_scroll_offset = 0;
  ed::better_timeline::register_builtin_types();
}

bool better_timeline_track_is_selected(const BetterTimelineTrack *track)
{
  return track != nullptr && track->selected != 0;
}

void better_timeline_track_set_selected(BetterTimelineTrack *track, const bool selected)
{
  if (track != nullptr) {
    track->selected = selected ? 1 : 0;
  }
}

BetterTimelineTrack *better_timeline_track_at_index(SpaceBetterTimeline *sbetter_timeline,
                                                    const int track_index)
{
  if (sbetter_timeline == nullptr || track_index < 0) {
    return nullptr;
  }
  return static_cast<BetterTimelineTrack *>(BLI_findlink(&sbetter_timeline->tracks, track_index));
}

const BetterTimelineTrack *better_timeline_track_at_index(
    const SpaceBetterTimeline *sbetter_timeline, const int track_index)
{
  if (sbetter_timeline == nullptr || track_index < 0) {
    return nullptr;
  }
  return static_cast<const BetterTimelineTrack *>(
      BLI_findlink(&sbetter_timeline->tracks, track_index));
}

int better_timeline_track_count(const SpaceBetterTimeline *sbetter_timeline)
{
  return (sbetter_timeline != nullptr) ? BLI_listbase_count(&sbetter_timeline->tracks) : 0;
}

int better_timeline_track_index_from_ptr(const SpaceBetterTimeline *sbetter_timeline,
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

BetterTimelineTrack *better_timeline_track_create(const char *track_type_idname,
                                                  const int track_name_index)
{
  auto *track = MEM_new<BetterTimelineTrack>(__func__);
  SNPRINTF(track->name, "Track%d", std::max(1, track_name_index));
  if (track_type_idname != nullptr) {
    STRNCPY_UTF8(track->track_type, track_type_idname);
  }
  better_timeline_track_ensure_type(track);
  track->selected = 0;
  return track;
}

void better_timeline_track_free(BetterTimelineTrack *track)
{
  if (track == nullptr) {
    return;
  }

  better_timeline_clips_free(&track->clips);
  if (track->properties != nullptr) {
    IDP_FreeProperty(track->properties);
    track->properties = nullptr;
  }
  MEM_delete(track);
}

void better_timeline_tracks_free(ListBase *tracks)
{
  if (tracks == nullptr) {
    return;
  }
  BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(tracks->first);
  while (track != nullptr) {
    BetterTimelineTrack *track_next = track->next;
    better_timeline_track_free(track);
    track = track_next;
  }

  BLI_listbase_clear(tracks);
}

void better_timeline_tracks_duplicate(ListBase *dst, const ListBase *src)
{
  BLI_listbase_clear(dst);
  if (src == nullptr) {
    return;
  }

  for (const BetterTimelineTrack *track_src = static_cast<const BetterTimelineTrack *>(src->first);
       track_src != nullptr;
       track_src = track_src->next)
  {
    auto *track_dst = MEM_new<BetterTimelineTrack>(__func__);
    *track_dst = *track_src;
    track_dst->next = nullptr;
    track_dst->prev = nullptr;
    BLI_listbase_clear(&track_dst->clips);
    better_timeline_clips_duplicate(&track_dst->clips, &track_src->clips);
    track_dst->properties = (track_src->properties != nullptr) ? IDP_CopyProperty(track_src->properties) :
                                                                  nullptr;
    better_timeline_track_ensure_type(track_dst);
    for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track_dst->clips.first);
         clip != nullptr;
         clip = clip->next)
    {
      better_timeline_clip_ensure_type(track_dst, clip);
    }
    BLI_addtail(dst, track_dst);
  }
}

const char *better_timeline_track_type_label_get(const BetterTimelineTrack *track)
{
  if (const ed::better_timeline::BetterTimelineTrackType *track_type =
          better_timeline_track_type_get(track))
  {
    return track_type->label[0] != '\0' ? track_type->label : track_type->idname;
  }
  return "Unknown Track Type";
}

void better_timeline_track_ensure_type(BetterTimelineTrack *track)
{
  if (track == nullptr) {
    return;
  }

  if (track->track_type[0] != '\0' &&
      ed::better_timeline::track_type_find_from_idname(track->track_type) != nullptr)
  {
    return;
  }

  if (const ed::better_timeline::BetterTimelineTrackType *default_track_type =
          better_timeline_default_track_type())
  {
    STRNCPY_UTF8(track->track_type, default_track_type->idname);
  }
}

void better_timeline_clip_ensure_type(BetterTimelineTrack *track, BetterTimelineClip *clip)
{
  if (track == nullptr || clip == nullptr) {
    return;
  }

  if (clip->clip_type[0] != '\0' && ed::better_timeline::track_accepts_clip(*track, *clip)) {
    return;
  }

  if (const ed::better_timeline::BetterTimelineTrackType *track_type =
          better_timeline_track_type_get(track))
  {
    const Vector<const ed::better_timeline::BetterTimelineClipType *> compatible_clip_types =
        ed::better_timeline::compatible_clip_types(*track_type);
    if (!compatible_clip_types.is_empty()) {
      STRNCPY_UTF8(clip->clip_type, compatible_clip_types.first()->idname);
    }
  }
}

bool better_timeline_has_selected_track(const SpaceBetterTimeline *sbetter_timeline)
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

int better_timeline_selected_track_count(const SpaceBetterTimeline *sbetter_timeline)
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

void better_timeline_clear_selection(SpaceBetterTimeline *sbetter_timeline)
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

void better_timeline_select_only_track(SpaceBetterTimeline *sbetter_timeline, const int track_index)
{
  better_timeline_clear_selection(sbetter_timeline);

  BetterTimelineTrack *track = better_timeline_track_at_index(sbetter_timeline, track_index);
  if (track != nullptr) {
    better_timeline_track_set_selected(track, true);
    sbetter_timeline->selected_track_index = track_index;
  }
}

int better_timeline_first_selected_track_index(const SpaceBetterTimeline *sbetter_timeline)
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

void better_timeline_space_blend_read_data(BlendDataReader *reader, SpaceLink *sl)
{
  auto *sbetter_timeline = reinterpret_cast<SpaceBetterTimeline *>(sl);
  ed::better_timeline::register_builtin_types();

  BLO_read_struct_list(reader, BetterTimelineTrack, &sbetter_timeline->tracks);
  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    BLO_read_struct(reader, IDProperty, &track->properties);
    IDP_BlendDataRead(reader, &track->properties);

    BLO_read_struct_list(reader, BetterTimelineClip, &track->clips);
    better_timeline_track_ensure_type(track);

    for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first);
         clip != nullptr;
         clip = clip->next)
    {
      BLO_read_struct(reader, IDProperty, &clip->properties);
      IDP_BlendDataRead(reader, &clip->properties);
      better_timeline_clip_ensure_type(track, clip);
    }
  }

  better_timeline_state_normalize_after_read(sbetter_timeline);
}

void better_timeline_space_blend_write(BlendWriter *writer, SpaceLink *sl)
{
  auto *sbetter_timeline = reinterpret_cast<SpaceBetterTimeline *>(sl);

  for (const BetterTimelineTrack *track = static_cast<const BetterTimelineTrack *>(
           sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    writer->write_struct(track);
    if (track->properties != nullptr) {
      IDP_BlendWrite(writer, track->properties);
    }

    for (const BetterTimelineClip *clip = static_cast<const BetterTimelineClip *>(track->clips.first);
         clip != nullptr;
         clip = clip->next)
    {
      writer->write_struct(clip);
      if (clip->properties != nullptr) {
        IDP_BlendWrite(writer, clip->properties);
      }
    }
  }
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

}  // namespace blender
