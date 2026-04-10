/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include <memory>
#include <string>

#include "BLI_function_ref.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "RNA_types.hh"

namespace blender {

struct BetterTimelineClip;
struct BetterTimelineTrack;

namespace ed::better_timeline {

constexpr int BETTER_TIMELINE_TYPE_IDNAME_MAX = 64;

enum class BetterTimelineClipBlendMode {
  Solid = 0,
  Blendable = 1,
};

struct BetterTimelineClipType {
  char idname[BETTER_TIMELINE_TYPE_IDNAME_MAX] = "";
  char label[BETTER_TIMELINE_TYPE_IDNAME_MAX] = "";
  std::string description;
  BetterTimelineClipBlendMode blend_mode = BetterTimelineClipBlendMode::Solid;
  ExtensionRNA rna_ext = {};
};

struct BetterTimelineTrackType {
  char idname[BETTER_TIMELINE_TYPE_IDNAME_MAX] = "";
  char label[BETTER_TIMELINE_TYPE_IDNAME_MAX] = "";
  std::string description;
  Vector<std::string> compatible_clip_type_ids;
  bool (*clip_type_poll)(const BetterTimelineTrackType *track_type,
                         const BetterTimelineClipType *clip_type) = nullptr;
  ExtensionRNA rna_ext = {};
  /** RGB accent color for the left stripe and track type icon tint. */
  float color[3] = {0.5f, 0.5f, 0.5f};
  /** BIFIconID for the track type icon shown in the track list pane. */
  int icon = 0;
  /** If true, this track type supports binding a scene Object to the track.
   *  The bound object is stored in `BetterTimelineTrack::object`. */
  bool has_object_slot = false;
};

void register_builtin_types();

void track_type_register(std::unique_ptr<BetterTimelineTrackType> track_type);
void track_type_unregister(const BetterTimelineTrackType &track_type);
void clip_type_register(std::unique_ptr<BetterTimelineClipType> clip_type);
void clip_type_unregister(const BetterTimelineClipType &clip_type);

BetterTimelineTrackType *track_type_find_from_idname(StringRef idname);
BetterTimelineClipType *clip_type_find_from_idname(StringRef idname);
const BetterTimelineTrackType *default_track_type_get();

void foreach_track_type(FunctionRef<void(const BetterTimelineTrackType &track_type)> fn);
void foreach_clip_type(FunctionRef<void(const BetterTimelineClipType &clip_type)> fn);

bool track_type_accepts_clip_type(const BetterTimelineTrackType &track_type,
                                  const BetterTimelineClipType &clip_type);
bool track_type_accepts_clip_type(StringRef track_type_idname, StringRef clip_type_idname);
bool track_accepts_clip_type(const BetterTimelineTrack &track, StringRef clip_type_idname);
bool track_accepts_clip(const BetterTimelineTrack &track, const BetterTimelineClip &clip);
bool track_can_place_clip(const BetterTimelineTrack &track,
                          StringRef clip_type_idname,
                          float start_frame,
                          float end_frame,
                          const BetterTimelineClip *ignore_clip = nullptr);
BetterTimelineClipBlendMode clip_type_blend_mode(const BetterTimelineClipType &clip_type);
BetterTimelineClipBlendMode clip_type_blend_mode(StringRef clip_type_idname);
bool clip_type_is_blendable(StringRef clip_type_idname);
bool clip_types_allow_overlap(StringRef clip_type_idname_a, StringRef clip_type_idname_b);

Vector<const BetterTimelineClipType *> compatible_clip_types(
    const BetterTimelineTrackType &track_type);

}  // namespace ed::better_timeline
}  // namespace blender
