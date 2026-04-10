/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#include <algorithm>
#include <initializer_list>
#include <memory>

#include "DNA_space_types.h"

#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "ED_better_timeline.hh"
#include "UI_resources.hh"

namespace blender::ed::better_timeline {

static constexpr const char *BETTER_TIMELINE_TRACK_TYPE_TEST = "BETTER_TIMELINE_TT_TEST";
static constexpr const char *BETTER_TIMELINE_TRACK_TYPE_ANIMATION =
    "BETTER_TIMELINE_TT_ANIMATION";
static constexpr const char *BETTER_TIMELINE_TRACK_TYPE_SPLINE = "BETTER_TIMELINE_TT_SPLINE";
static constexpr const char *BETTER_TIMELINE_CLIP_TYPE_TEST = "BETTER_TIMELINE_CT_TEST";
static constexpr const char *BETTER_TIMELINE_CLIP_TYPE_ANIMATION =
    "BETTER_TIMELINE_CT_ANIMATION";
static constexpr const char *BETTER_TIMELINE_CLIP_TYPE_SPLINE = "BETTER_TIMELINE_CT_SPLINE";

static Vector<std::unique_ptr<BetterTimelineTrackType>> &static_track_types()
{
  static Vector<std::unique_ptr<BetterTimelineTrackType>> track_types;
  return track_types;
}

static Vector<std::unique_ptr<BetterTimelineClipType>> &static_clip_types()
{
  static Vector<std::unique_ptr<BetterTimelineClipType>> clip_types;
  return clip_types;
}

static bool builtin_types_registered = false;

static void ensure_builtin_types_registered()
{
  if (builtin_types_registered) {
    return;
  }

  builtin_types_registered = true;

  auto add_clip_type = [](const char *idname,
                          const char *label,
                          const char *description,
                          const BetterTimelineClipBlendMode blend_mode) {
    auto clip_type = std::make_unique<BetterTimelineClipType>();
    STRNCPY_UTF8(clip_type->idname, idname);
    STRNCPY_UTF8(clip_type->label, label);
    clip_type->description = description;
    clip_type->blend_mode = blend_mode;
    clip_type_register(std::move(clip_type));
  };

  auto add_track_type = [](const char *idname,
                           const char *label,
                           const char *description,
                           std::initializer_list<const char *> compatible_clip_type_ids,
                           const float color[3],
                           const int icon,
                           const bool has_object_slot = false) {
    auto track_type = std::make_unique<BetterTimelineTrackType>();
    STRNCPY_UTF8(track_type->idname, idname);
    STRNCPY_UTF8(track_type->label, label);
    track_type->description = description;
    for (const char *clip_type_idname : compatible_clip_type_ids) {
      track_type->compatible_clip_type_ids.append(clip_type_idname);
    }
    track_type->color[0] = color[0];
    track_type->color[1] = color[1];
    track_type->color[2] = color[2];
    track_type->icon = icon;
    track_type->has_object_slot = has_object_slot;
    track_type_register(std::move(track_type));
  };

  add_clip_type(BETTER_TIMELINE_CLIP_TYPE_TEST,
                "Test Clip",
                "Placeholder clip type for early Better Timeline development",
                BetterTimelineClipBlendMode::Solid);
  add_clip_type(BETTER_TIMELINE_CLIP_TYPE_ANIMATION,
                "Animation Clip",
                "Animation data clip for animation-oriented tracks",
                BetterTimelineClipBlendMode::Blendable);
  add_clip_type(BETTER_TIMELINE_CLIP_TYPE_SPLINE,
                "Spline Clip",
                "Spline data clip for spline-oriented tracks",
                BetterTimelineClipBlendMode::Blendable);

  {
    static const float test_color[3] = {0.38f, 0.51f, 0.68f};
    add_track_type(BETTER_TIMELINE_TRACK_TYPE_TEST,
                   "Test Track",
                   "Placeholder track type for early Better Timeline development",
                   {BETTER_TIMELINE_CLIP_TYPE_TEST},
                   test_color,
                   ICON_SEQ_SEQUENCER);
  }
  {
    static const float anim_color[3] = {0.35f, 0.62f, 0.43f};
    add_track_type(BETTER_TIMELINE_TRACK_TYPE_ANIMATION,
                   "Animation Track",
                   "Track type that accepts animation clips",
                   {BETTER_TIMELINE_CLIP_TYPE_ANIMATION},
                   anim_color,
                   ICON_ACTION,
                   true);
  }
  {
    static const float spline_color[3] = {0.72f, 0.52f, 0.22f};
    add_track_type(BETTER_TIMELINE_TRACK_TYPE_SPLINE,
                   "Spline Track",
                   "Track type that accepts spline clips",
                   {BETTER_TIMELINE_CLIP_TYPE_SPLINE},
                   spline_color,
                   ICON_CURVE_DATA);
  }
}

void register_builtin_types()
{
  ensure_builtin_types_registered();
}

void track_type_register(std::unique_ptr<BetterTimelineTrackType> track_type)
{
  ensure_builtin_types_registered();
  BLI_assert(track_type != nullptr);
  BLI_assert(track_type_find_from_idname(track_type->idname) == nullptr);
  static_track_types().append(std::move(track_type));
}

void track_type_unregister(const BetterTimelineTrackType &track_type)
{
  ensure_builtin_types_registered();
  Vector<std::unique_ptr<BetterTimelineTrackType>> &track_types = static_track_types();
  const auto it = std::find_if(track_types.begin(),
                               track_types.end(),
                               [&](const std::unique_ptr<BetterTimelineTrackType> &iter_type) {
                                 return iter_type.get() == &track_type;
                               });
  BLI_assert(it != track_types.end());
  if (it != track_types.end()) {
    track_types.remove(it - track_types.begin());
  }
}

void clip_type_register(std::unique_ptr<BetterTimelineClipType> clip_type)
{
  ensure_builtin_types_registered();
  BLI_assert(clip_type != nullptr);
  BLI_assert(clip_type_find_from_idname(clip_type->idname) == nullptr);
  static_clip_types().append(std::move(clip_type));
}

void clip_type_unregister(const BetterTimelineClipType &clip_type)
{
  ensure_builtin_types_registered();
  Vector<std::unique_ptr<BetterTimelineClipType>> &clip_types = static_clip_types();
  const auto it = std::find_if(clip_types.begin(),
                               clip_types.end(),
                               [&](const std::unique_ptr<BetterTimelineClipType> &iter_type) {
                                 return iter_type.get() == &clip_type;
                               });
  BLI_assert(it != clip_types.end());
  if (it != clip_types.end()) {
    clip_types.remove(it - clip_types.begin());
  }
}

BetterTimelineTrackType *track_type_find_from_idname(const StringRef idname)
{
  ensure_builtin_types_registered();
  for (const std::unique_ptr<BetterTimelineTrackType> &track_type : static_track_types()) {
    if (idname == track_type->idname) {
      return track_type.get();
    }
  }
  return nullptr;
}

BetterTimelineClipType *clip_type_find_from_idname(const StringRef idname)
{
  ensure_builtin_types_registered();
  for (const std::unique_ptr<BetterTimelineClipType> &clip_type : static_clip_types()) {
    if (idname == clip_type->idname) {
      return clip_type.get();
    }
  }
  return nullptr;
}

const BetterTimelineTrackType *default_track_type_get()
{
  ensure_builtin_types_registered();
  return track_type_find_from_idname(BETTER_TIMELINE_TRACK_TYPE_TEST);
}

void foreach_track_type(const FunctionRef<void(const BetterTimelineTrackType &track_type)> fn)
{
  ensure_builtin_types_registered();
  for (const std::unique_ptr<BetterTimelineTrackType> &track_type : static_track_types()) {
    fn(*track_type);
  }
}

void foreach_clip_type(const FunctionRef<void(const BetterTimelineClipType &clip_type)> fn)
{
  ensure_builtin_types_registered();
  for (const std::unique_ptr<BetterTimelineClipType> &clip_type : static_clip_types()) {
    fn(*clip_type);
  }
}

bool track_type_accepts_clip_type(const BetterTimelineTrackType &track_type,
                                  const BetterTimelineClipType &clip_type)
{
  if (track_type.clip_type_poll != nullptr) {
    return track_type.clip_type_poll(&track_type, &clip_type);
  }

  return std::any_of(track_type.compatible_clip_type_ids.begin(),
                     track_type.compatible_clip_type_ids.end(),
                     [&](const std::string &compatible_clip_type_idname) {
                       return compatible_clip_type_idname == clip_type.idname;
                     });
}

bool track_type_accepts_clip_type(const StringRef track_type_idname, const StringRef clip_type_idname)
{
  const BetterTimelineTrackType *track_type = track_type_find_from_idname(track_type_idname);
  const BetterTimelineClipType *clip_type = clip_type_find_from_idname(clip_type_idname);
  return track_type != nullptr && clip_type != nullptr &&
         track_type_accepts_clip_type(*track_type, *clip_type);
}

bool track_accepts_clip_type(const BetterTimelineTrack &track, const StringRef clip_type_idname)
{
  return track_type_accepts_clip_type(track.track_type, clip_type_idname);
}

bool track_accepts_clip(const BetterTimelineTrack &track, const BetterTimelineClip &clip)
{
  return track_type_accepts_clip_type(track.track_type, clip.clip_type);
}

BetterTimelineClipBlendMode clip_type_blend_mode(const BetterTimelineClipType &clip_type)
{
  return clip_type.blend_mode;
}

BetterTimelineClipBlendMode clip_type_blend_mode(const StringRef clip_type_idname)
{
  const BetterTimelineClipType *clip_type = clip_type_find_from_idname(clip_type_idname);
  return (clip_type != nullptr) ? clip_type_blend_mode(*clip_type) :
                                  BetterTimelineClipBlendMode::Solid;
}

bool clip_type_is_blendable(const StringRef clip_type_idname)
{
  return clip_type_blend_mode(clip_type_idname) == BetterTimelineClipBlendMode::Blendable;
}

bool clip_types_allow_overlap(const StringRef clip_type_idname_a, const StringRef clip_type_idname_b)
{
  return clip_type_is_blendable(clip_type_idname_a) && clip_type_is_blendable(clip_type_idname_b);
}

Vector<const BetterTimelineClipType *> compatible_clip_types(const BetterTimelineTrackType &track_type)
{
  ensure_builtin_types_registered();
  Vector<const BetterTimelineClipType *> result;
  for (const std::unique_ptr<BetterTimelineClipType> &clip_type : static_clip_types()) {
    if (track_type_accepts_clip_type(track_type, *clip_type)) {
      result.append(clip_type.get());
    }
  }
  return result;
}

}  // namespace blender::ed::better_timeline
