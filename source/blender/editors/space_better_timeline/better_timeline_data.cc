/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#include <algorithm>
#include <cstdio>
#include <utility>

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

static void better_timeline_space_runtime_ensure(SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline != nullptr && sbetter_timeline->runtime == nullptr) {
    sbetter_timeline->runtime = MEM_new<SpaceBetterTimeline_Runtime>(__func__);
  }
}

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

BetterTimelineClip *better_timeline_clip_duplicate(const BetterTimelineClip *clip_src)
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

static const ed::better_timeline::BetterTimelineClipType *better_timeline_clip_type_get(
    const BetterTimelineClip *clip)
{
  if (clip == nullptr || clip->clip_type[0] == '\0') {
    return nullptr;
  }
  return ed::better_timeline::clip_type_find_from_idname(clip->clip_type);
}

static StringRef better_timeline_duplicate_name_root(StringRef name)
{
  const int suffix_len = 4;
  if (name.size() <= suffix_len) {
    return name;
  }

  const StringRef suffix = name.substr(name.size() - suffix_len);
  if (suffix[0] != ' ' || !isdigit(suffix[1]) || !isdigit(suffix[2]) || !isdigit(suffix[3])) {
    return name;
  }
  return name.drop_suffix(suffix_len);
}

template<typename MatchFn>
static void better_timeline_assign_incremental_duplicate_name(char *dst_name,
                                                              const size_t dst_name_capacity,
                                                              const StringRef source_name,
                                                              const MatchFn &match_name)
{
  const std::string root = better_timeline_duplicate_name_root(source_name).trim().is_empty() ?
                               std::string(source_name) :
                               std::string(better_timeline_duplicate_name_root(source_name));

  int max_suffix = 0;
  char candidate[64];
  for (int suffix = 1; suffix < 1000; suffix++) {
    SNPRINTF(candidate, "%s %03d", root.c_str(), suffix);
    if (!match_name(candidate)) {
      BLI_strncpy_utf8(dst_name, candidate, dst_name_capacity);
      return;
    }
    max_suffix = suffix;
  }

  SNPRINTF(candidate, "%s %03d", root.c_str(), max_suffix + 1);
  BLI_strncpy_utf8(dst_name, candidate, dst_name_capacity);
}

static void better_timeline_state_clear(BetterTimelineUndoState *state)
{
  if (state == nullptr) {
    return;
  }

  better_timeline_tracks_free(&state->tracks);
  state->selected_track_index = -1;
  state->selected_clip_index = -1;
  state->next_track_name_index = 1;
  state->track_panel_width = 0;
  state->track_scroll_offset = 0;
}

static void better_timeline_state_snapshot_from_space(BetterTimelineUndoState *state,
                                                      const SpaceBetterTimeline *sbetter_timeline)
{
  BLI_assert(state != nullptr);
  BLI_assert(sbetter_timeline != nullptr);

  better_timeline_state_clear(state);
  better_timeline_tracks_duplicate(&state->tracks, &sbetter_timeline->tracks);
  state->selected_track_index = sbetter_timeline->selected_track_index;
  state->selected_clip_index = sbetter_timeline->selected_clip_index;
  state->next_track_name_index = sbetter_timeline->next_track_name_index;
  state->track_panel_width = sbetter_timeline->track_panel_width;
  state->track_scroll_offset = sbetter_timeline->track_scroll_offset;
}

static void better_timeline_state_restore(SpaceBetterTimeline *dst,
                                          const BetterTimelineUndoState *state)
{
  if (dst == nullptr || state == nullptr) {
    return;
  }

  better_timeline_tracks_free(&dst->tracks);
  better_timeline_tracks_duplicate(&dst->tracks, &state->tracks);
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
  dst->selected_track_index = state->selected_track_index;
  dst->selected_clip_index = state->selected_clip_index;
  dst->next_track_name_index = std::max(1, state->next_track_name_index);
  dst->track_panel_width = state->track_panel_width;
  dst->track_scroll_offset = std::max(0, state->track_scroll_offset);
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

  if (!better_timeline_has_selected_clip(sbetter_timeline)) {
    sbetter_timeline->selected_clip_index = -1;
  }
  else if (BetterTimelineClip *active_clip = better_timeline_clip_at_global_index(
               sbetter_timeline, sbetter_timeline->selected_clip_index, nullptr))
  {
    if (!better_timeline_clip_is_selected(active_clip)) {
      sbetter_timeline->selected_clip_index = better_timeline_first_selected_clip_index(
          sbetter_timeline);
    }
  }
  else {
    sbetter_timeline->selected_clip_index = better_timeline_first_selected_clip_index(
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

static void better_timeline_undosys_step_encode_init(bContext *C, UndoStep *us_p)
{
  auto *us = reinterpret_cast<BetterTimelineUndoStep *>(us_p);
  auto *sbetter_timeline = reinterpret_cast<SpaceBetterTimeline *>(CTX_wm_space_data(C));
  if (sbetter_timeline == nullptr) {
    return;
  }

  us->space = sbetter_timeline;
  better_timeline_state_snapshot_from_space(&us->state_before, sbetter_timeline);
}

static bool better_timeline_undosys_step_encode(bContext *C, Main * /*bmain*/, UndoStep *us_p)
{
  auto *us = reinterpret_cast<BetterTimelineUndoStep *>(us_p);
  auto *sbetter_timeline = reinterpret_cast<SpaceBetterTimeline *>(CTX_wm_space_data(C));
  if (sbetter_timeline == nullptr) {
    return false;
  }

  us->space = sbetter_timeline;
  better_timeline_state_snapshot_from_space(&us->state_after, sbetter_timeline);
  /* Better Timeline state lives on bScreen and is not restored by plain memfile undo steps.
   * Pair each track-state step with a memfile boundary so mixed scene/object undo preserves the
   * expected user-visible order instead of letting unrelated global steps jump ahead. */
  us->step.use_memfile_step = true;
  return true;
}

static void better_timeline_undosys_step_decode(
    bContext *C, Main * /*bmain*/, UndoStep *us_p, const eUndoStepDir dir, bool /*is_final*/)
{
  auto *us = reinterpret_cast<BetterTimelineUndoStep *>(us_p);
  if (us->space == nullptr) {
    return;
  }

  better_timeline_state_restore(
      us->space, (dir == STEP_UNDO) ? &us->state_before : &us->state_after);
  better_timeline_track_drag_visual_state_clear(us->space);
  WM_event_add_notifier(C, NC_SCREEN | NA_EDITED, nullptr);
}

static void better_timeline_undosys_step_free(UndoStep *us_p)
{
  auto *us = reinterpret_cast<BetterTimelineUndoStep *>(us_p);
  better_timeline_state_clear(&us->state_before);
  better_timeline_state_clear(&us->state_after);
}

void better_timeline_space_state_init(SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return;
  }

  better_timeline_space_runtime_ensure(sbetter_timeline);
  sbetter_timeline->selected_track_index = -1;
  sbetter_timeline->selected_clip_index = -1;
  sbetter_timeline->next_track_name_index = 1;
  sbetter_timeline->track_panel_width = 0;
  sbetter_timeline->track_scroll_offset = 0;
  ed::better_timeline::register_builtin_types();
}

void better_timeline_space_runtime_free(SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return;
  }
  MEM_SAFE_DELETE(sbetter_timeline->runtime);
}

bool better_timeline_track_is_selected(const BetterTimelineTrack *track)
{
  return track != nullptr && track->selected != 0;
}

bool better_timeline_track_is_muted(const BetterTimelineTrack *track)
{
  return track != nullptr && (track->flag & BETTER_TIMELINE_TRACK_MUTED) != 0;
}

bool better_timeline_track_is_locked(const BetterTimelineTrack *track)
{
  return track != nullptr && (track->flag & BETTER_TIMELINE_TRACK_LOCKED) != 0;
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

BetterTimelineTrack *better_timeline_track_duplicate(const BetterTimelineTrack *track_src)
{
  if (track_src == nullptr) {
    return nullptr;
  }

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
  return track_dst;
}

void better_timeline_track_assign_duplicate_name(const SpaceBetterTimeline *sbetter_timeline,
                                                 BetterTimelineTrack *track,
                                                 const StringRef source_name)
{
  if (sbetter_timeline == nullptr || track == nullptr) {
    return;
  }

  better_timeline_assign_incremental_duplicate_name(
      track->name,
      sizeof(track->name),
      source_name,
      [&](const StringRef candidate_name) {
        for (const BetterTimelineTrack *iter_track = static_cast<const BetterTimelineTrack *>(
                 sbetter_timeline->tracks.first);
             iter_track != nullptr;
             iter_track = iter_track->next)
        {
          if (candidate_name == iter_track->name) {
            return true;
          }
        }
        return false;
      });
}

BetterTimelineClip *better_timeline_clip_create(const BetterTimelineTrack *track,
                                                const char *clip_type_idname,
                                                const float start_frame,
                                                const float end_frame)
{
  if (track == nullptr || clip_type_idname == nullptr || clip_type_idname[0] == '\0' ||
      !ed::better_timeline::track_accepts_clip_type(*track, clip_type_idname))
  {
    return nullptr;
  }

  auto *clip = MEM_new<BetterTimelineClip>(__func__);
  STRNCPY_UTF8(clip->clip_type, clip_type_idname);
  if (const ed::better_timeline::BetterTimelineClipType *clip_type =
          better_timeline_clip_type_get(clip))
  {
    STRNCPY_UTF8(clip->name, clip_type->label);
  }
  clip->start_frame = start_frame;
  clip->end_frame = std::max(start_frame + 1.0f, end_frame);
  return clip;
}

bool better_timeline_clip_range_overlaps(const float start_frame_a,
                                         const float end_frame_a,
                                         const float start_frame_b,
                                         const float end_frame_b)
{
  return start_frame_a < end_frame_b && start_frame_b < end_frame_a;
}

static bool better_timeline_clip_is_ignored(const BetterTimelineClip *clip,
                                            const BetterTimelineClip *ignore_clip,
                                            const Span<const BetterTimelineClip *> ignored_clips)
{
  return clip == ignore_clip || ignored_clips.contains(clip);
}

bool better_timeline_track_can_place_clip(const BetterTimelineTrack *track,
                                          const StringRef clip_type_idname,
                                          const float start_frame,
                                          const float end_frame,
                                          const BetterTimelineClip *ignore_clip,
                                          const Span<const BetterTimelineClip *> ignored_clips)
{
  if (track == nullptr || clip_type_idname.is_empty() || end_frame <= start_frame ||
      !ed::better_timeline::track_accepts_clip_type(*track, clip_type_idname))
  {
    return false;
  }

  Vector<std::pair<float, int>> overlap_events;
  for (const BetterTimelineClip *clip = static_cast<const BetterTimelineClip *>(track->clips.first);
       clip != nullptr;
       clip = clip->next)
  {
    if (better_timeline_clip_is_ignored(clip, ignore_clip, ignored_clips) ||
        !better_timeline_clip_range_overlaps(start_frame, end_frame, clip->start_frame, clip->end_frame))
    {
      continue;
    }

    if (!ed::better_timeline::clip_types_allow_overlap(clip_type_idname, clip->clip_type)) {
      return false;
    }

    const float overlap_start = std::max(start_frame, clip->start_frame);
    const float overlap_end = std::min(end_frame, clip->end_frame);
    overlap_events.append({overlap_start, 1});
    overlap_events.append({overlap_end, -1});
  }

  std::sort(overlap_events.begin(), overlap_events.end(), [](const auto &a, const auto &b) {
    if (a.first == b.first) {
      return a.second < b.second;
    }
    return a.first < b.first;
  });

  int existing_overlap_depth = 0;
  for (const std::pair<float, int> &event : overlap_events) {
    existing_overlap_depth += event.second;
    if (existing_overlap_depth > 1) {
      return false;
    }
  }

  return true;
}

namespace ed::better_timeline {

bool track_can_place_clip(const BetterTimelineTrack &track,
                          const StringRef clip_type_idname,
                          const float start_frame,
                          const float end_frame,
                          const BetterTimelineClip *ignore_clip)
{
  return better_timeline_track_can_place_clip(
      &track, clip_type_idname, start_frame, end_frame, ignore_clip);
}

}  // namespace ed::better_timeline

const BetterTimelineClip *better_timeline_clip_covering_frame(const BetterTimelineTrack *track,
                                                              const float frame,
                                                              const StringRef clip_type_idname)
{
  if (track == nullptr) {
    return nullptr;
  }

  for (const BetterTimelineClip *clip = static_cast<const BetterTimelineClip *>(track->clips.first);
       clip != nullptr;
       clip = clip->next)
  {
    if (!(clip->start_frame <= frame && frame < clip->end_frame)) {
      continue;
    }
    if (!ed::better_timeline::clip_types_allow_overlap(clip_type_idname, clip->clip_type)) {
      return clip;
    }
  }
  return nullptr;
}

static float better_timeline_track_end_frame(const BetterTimelineTrack *track)
{
  float end_frame = 0.0f;
  if (track == nullptr) {
    return end_frame;
  }

  for (const BetterTimelineClip *clip = static_cast<const BetterTimelineClip *>(track->clips.first);
       clip != nullptr;
       clip = clip->next)
  {
    end_frame = std::max(end_frame, clip->end_frame);
  }
  return end_frame;
}

float better_timeline_clip_creation_start_frame(const BetterTimelineTrack *track,
                                                const StringRef clip_type_idname,
                                                const float requested_start_frame,
                                                const float duration_frames)
{
  if (track == nullptr) {
    return requested_start_frame;
  }

  const float end_frame = requested_start_frame + duration_frames;
  if (better_timeline_track_can_place_clip(
          track, clip_type_idname, requested_start_frame, end_frame))
  {
    return requested_start_frame;
  }

  if (const BetterTimelineClip *blocking_clip = better_timeline_clip_covering_frame(
          track, requested_start_frame, clip_type_idname))
  {
    const float shifted_start_frame = blocking_clip->end_frame;
    if (better_timeline_track_can_place_clip(track,
                                             clip_type_idname,
                                             shifted_start_frame,
                                             shifted_start_frame + duration_frames))
    {
      return shifted_start_frame;
    }
  }

  return better_timeline_track_end_frame(track);
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
    if (BetterTimelineTrack *track_dst = better_timeline_track_duplicate(track_src)) {
      BLI_addtail(dst, track_dst);
    }
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

const char *better_timeline_clip_type_label_get(const BetterTimelineClip *clip)
{
  if (const ed::better_timeline::BetterTimelineClipType *clip_type =
          better_timeline_clip_type_get(clip))
  {
    return clip_type->label[0] != '\0' ? clip_type->label : clip_type->idname;
  }
  return "Unknown Clip Type";
}

bool better_timeline_clip_is_selected(const BetterTimelineClip *clip)
{
  return clip != nullptr && clip->selected != 0;
}

void better_timeline_clip_set_selected(BetterTimelineClip *clip, const bool selected)
{
  if (clip != nullptr) {
    clip->selected = selected ? 1 : 0;
  }
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

void better_timeline_clip_assign_duplicate_name(const SpaceBetterTimeline *sbetter_timeline,
                                                BetterTimelineClip *clip,
                                                const StringRef source_name)
{
  if (sbetter_timeline == nullptr || clip == nullptr) {
    return;
  }

  better_timeline_assign_incremental_duplicate_name(
      clip->name,
      sizeof(clip->name),
      source_name,
      [&](const StringRef candidate_name) {
        for (const BetterTimelineTrack *track = static_cast<const BetterTimelineTrack *>(
                 sbetter_timeline->tracks.first);
             track != nullptr;
             track = track->next)
        {
          for (const BetterTimelineClip *iter_clip = static_cast<const BetterTimelineClip *>(
                   track->clips.first);
               iter_clip != nullptr;
               iter_clip = iter_clip->next)
          {
            if (candidate_name == iter_clip->name) {
              return true;
            }
          }
        }
        return false;
      });
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

bool better_timeline_has_selected_clip(const SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return false;
  }

  for (const BetterTimelineTrack *track = static_cast<const BetterTimelineTrack *>(
           sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    for (const BetterTimelineClip *clip = static_cast<const BetterTimelineClip *>(track->clips.first);
         clip != nullptr;
         clip = clip->next)
    {
      if (better_timeline_clip_is_selected(clip)) {
        return true;
      }
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

void better_timeline_clear_clip_selection(SpaceBetterTimeline *sbetter_timeline)
{
  if (sbetter_timeline == nullptr) {
    return;
  }

  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first);
         clip != nullptr;
         clip = clip->next)
    {
      better_timeline_clip_set_selected(clip, false);
    }
  }
  sbetter_timeline->selected_clip_index = -1;
}

void better_timeline_clear_clip_selection_for_track(SpaceBetterTimeline *sbetter_timeline,
                                                    BetterTimelineTrack *track)
{
  if (sbetter_timeline == nullptr || track == nullptr) {
    return;
  }

  for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first);
       clip != nullptr;
       clip = clip->next)
  {
    better_timeline_clip_set_selected(clip, false);
  }
  sbetter_timeline->selected_clip_index = better_timeline_first_selected_clip_index(
      sbetter_timeline);
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

int better_timeline_first_selected_clip_index(const SpaceBetterTimeline *sbetter_timeline)
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
    for (const BetterTimelineClip *clip = static_cast<const BetterTimelineClip *>(track->clips.first);
         clip != nullptr;
         clip = clip->next, index++)
    {
      if (better_timeline_clip_is_selected(clip)) {
        return index;
      }
    }
  }
  return -1;
}

int better_timeline_clip_global_index_from_ptr(const SpaceBetterTimeline *sbetter_timeline,
                                               const BetterTimelineTrack *track,
                                               const BetterTimelineClip *clip)
{
  if (sbetter_timeline == nullptr || track == nullptr || clip == nullptr) {
    return -1;
  }

  int index = 0;
  for (const BetterTimelineTrack *iter_track = static_cast<const BetterTimelineTrack *>(
           sbetter_timeline->tracks.first);
       iter_track != nullptr;
       iter_track = iter_track->next)
  {
    for (const BetterTimelineClip *iter_clip = static_cast<const BetterTimelineClip *>(
             iter_track->clips.first);
         iter_clip != nullptr;
         iter_clip = iter_clip->next, index++)
    {
      if (iter_track == track && iter_clip == clip) {
        return index;
      }
    }
  }
  return -1;
}

BetterTimelineClip *better_timeline_clip_at_global_index(SpaceBetterTimeline *sbetter_timeline,
                                                         const int clip_index,
                                                         BetterTimelineTrack **r_track)
{
  if (r_track != nullptr) {
    *r_track = nullptr;
  }
  if (sbetter_timeline == nullptr || clip_index < 0) {
    return nullptr;
  }

  int index = 0;
  for (BetterTimelineTrack *track = static_cast<BetterTimelineTrack *>(sbetter_timeline->tracks.first);
       track != nullptr;
       track = track->next)
  {
    for (BetterTimelineClip *clip = static_cast<BetterTimelineClip *>(track->clips.first);
         clip != nullptr;
         clip = clip->next, index++)
    {
      if (index == clip_index) {
        if (r_track != nullptr) {
          *r_track = track;
        }
        return clip;
      }
    }
  }
  return nullptr;
}

void better_timeline_space_blend_read_data(BlendDataReader *reader, SpaceLink *sl)
{
  auto *sbetter_timeline = reinterpret_cast<SpaceBetterTimeline *>(sl);
  sbetter_timeline->runtime = MEM_new<SpaceBetterTimeline_Runtime>(__func__);
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
  ut->step_encode_init = better_timeline_undosys_step_encode_init;
  ut->step_encode = better_timeline_undosys_step_encode;
  ut->step_decode = better_timeline_undosys_step_decode;
  ut->step_free = better_timeline_undosys_step_free;
  ut->flags = UNDOTYPE_FLAG_NEED_CONTEXT_FOR_ENCODE;
  ut->step_size = sizeof(BetterTimelineUndoStep);
}

}  // namespace blender
