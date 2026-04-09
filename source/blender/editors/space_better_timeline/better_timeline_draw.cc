/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#include <algorithm>
#include <cmath>

#include "DNA_space_types.h"

#include "BLI_math_base.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"

#include "ED_anim_api.hh"
#include "ED_better_timeline.hh"
#include "ED_screen.hh"
#include "ED_time_scrub_ui.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "BLF_api.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "better_timeline_intern.hh" /* own include */

namespace blender {

struct BetterTimelineClipState {
  int scissor[4];
};

static SpaceBetterTimeline_Runtime *better_timeline_runtime_get(
    const SpaceBetterTimeline *sbetter_timeline)
{
  return (sbetter_timeline != nullptr) ? sbetter_timeline->runtime : nullptr;
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
  if ((major_line_distance * 0.5f * pixels_per_frame) >= min_minor_px && major_line_distance > 1.0f)
  {
    draw_lines(major_line_distance / 2.0f, minor_color);
  }

  uchar major_color[3];
  ui::theme::get_color_3ubv(TH_GRID, major_color);
  draw_lines(major_line_distance, major_color);

  GPU_matrix_pop_projection();
}

static void better_timeline_clip_color_get(const BetterTimelineClip *clip, float color[4])
{
  color[0] = 0.38f;
  color[1] = 0.51f;
  color[2] = 0.68f;
  color[3] = 0.95f;

  if (clip == nullptr) {
    return;
  }

  if (STREQ(clip->clip_type, "BETTER_TIMELINE_CT_TEST")) {
    color[0] = 0.38f;
    color[1] = 0.51f;
    color[2] = 0.68f;
  }
  else if (STREQ(clip->clip_type, "BETTER_TIMELINE_CT_ANIMATION")) {
    color[0] = 0.35f;
    color[1] = 0.62f;
    color[2] = 0.43f;
  }
  else if (STREQ(clip->clip_type, "BETTER_TIMELINE_CT_SPLINE")) {
    color[0] = 0.78f;
    color[1] = 0.52f;
    color[2] = 0.28f;
  }

  if (better_timeline_clip_is_selected(clip)) {
    color[3] = 1.0f;
  }
}

/** Draw 45° diagonal stripe bands (bottom-left → top-right) across [x0,y0]→[x1,y1].
 *  Call with a program already bound and colour already set.
 *  The active scissor rect will clip any overflow. */
static void better_timeline_draw_diagonal_stripes(const float x0,
                                                  const float y0,
                                                  const float x1,
                                                  const float y1,
                                                  const uint pos)
{
  const float h = y1 - y0;
  if (h <= 0.0f || x1 <= x0) {
    return;
  }

  const float stripe_width = 50.0f * UI_SCALE_FAC;
  const float gap_width = 50.0f * UI_SCALE_FAC;
  const float pattern_width = stripe_width + gap_width;
  const float first_bx = floorf((x0 - h) / pattern_width) * pattern_width;
  const int stripe_count = int(ceilf((x1 - first_bx) / pattern_width)) + 2;
  if (stripe_count <= 0) {
    return;
  }

  immBegin(GPU_PRIM_TRIS, stripe_count * 6);
  for (int i = 0; i < stripe_count; i++) {
    const float bx0 = first_bx + float(i) * pattern_width;
    const float bx1 = bx0 + stripe_width;

    immVertex2f(pos, bx0, y0);
    immVertex2f(pos, bx1, y0);
    immVertex2f(pos, bx1 + h, y1);

    immVertex2f(pos, bx0, y0);
    immVertex2f(pos, bx1 + h, y1);
    immVertex2f(pos, bx0 + h, y1);
  }
  immEnd();
}

static void better_timeline_clip_rect_pixel_snap(float *r_start_x,
                                                 float *r_end_x,
                                                 float *r_clip_y_min,
                                                 float *r_clip_y_max)
{
  BLI_assert(r_start_x != nullptr);
  BLI_assert(r_end_x != nullptr);
  BLI_assert(r_clip_y_min != nullptr);
  BLI_assert(r_clip_y_max != nullptr);

  *r_start_x = std::floor(*r_start_x) + 0.5f;
  *r_end_x = std::floor(*r_end_x) + 0.5f;
  *r_clip_y_min = std::floor(*r_clip_y_min) + 0.5f;
  *r_clip_y_max = std::floor(*r_clip_y_max) + 0.5f;

  if (*r_end_x <= *r_start_x) {
    *r_end_x = *r_start_x + 1.0f;
  }
  if (*r_clip_y_max <= *r_clip_y_min) {
    *r_clip_y_max = *r_clip_y_min + 1.0f;
  }
}

static void better_timeline_clip_outline_color_get(const BetterTimelineClip *clip, float color[4])
{
  if (better_timeline_clip_is_selected(clip)) {
    color[0] = 0.98f;
    color[1] = 0.98f;
    color[2] = 0.98f;
    color[3] = 0.95f;
    return;
  }

  color[0] = 0.02f;
  color[1] = 0.02f;
  color[2] = 0.02f;
  color[3] = 0.65f;
}

static void better_timeline_draw_clip_outline(const float start_x,
                                              const float end_x,
                                              const float clip_y_min,
                                              const float clip_y_max,
                                              const uint pos,
                                              const float color[4],
                                              const bool selected)
{
  immUniformColor4f(color[0], color[1], color[2], color[3]);
  GPU_line_width(1.0f);
  immBegin(GPU_PRIM_LINES, selected ? 8 : 6);
  immVertex2f(pos, start_x, clip_y_max);
  immVertex2f(pos, end_x, clip_y_max);
  immVertex2f(pos, end_x, clip_y_max);
  immVertex2f(pos, end_x, clip_y_min);
  immVertex2f(pos, start_x, clip_y_min);
  immVertex2f(pos, start_x, clip_y_max);
  if (selected) {
    immVertex2f(pos, start_x, clip_y_min);
    immVertex2f(pos, end_x, clip_y_min);
  }
  immEnd();
}

static void better_timeline_draw_clip_bottom_accent(const float start_x,
                                                    const float end_x,
                                                    const float clip_y_min,
                                                    const uint pos,
                                                    const float clip_color[4])
{
  const float accent_color[4] = {
      std::max(clip_color[0] - 0.18f, 0.0f),
      std::max(clip_color[1] - 0.18f, 0.0f),
      std::max(clip_color[2] - 0.18f, 0.0f),
      0.95f,
  };
  immUniformColor4f(
      accent_color[0], accent_color[1], accent_color[2], accent_color[3]);
  immRectf(pos, start_x, clip_y_min, end_x, clip_y_min + (2.0f * UI_SCALE_FAC));
}

static void better_timeline_draw_clip_selection_highlight(const float start_x,
                                                          const float end_x,
                                                          const float clip_y_min,
                                                          const float clip_y_max,
                                                          const uint pos)
{
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.18f);
  immRectf(pos,
           start_x + (1.0f * UI_SCALE_FAC),
           clip_y_max - (2.0f * UI_SCALE_FAC),
           end_x - (1.0f * UI_SCALE_FAC),
           clip_y_max - (1.0f * UI_SCALE_FAC));

  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.10f);
  immRectf(pos,
           start_x + (1.0f * UI_SCALE_FAC),
           clip_y_min + (1.0f * UI_SCALE_FAC),
           start_x + (2.0f * UI_SCALE_FAC),
           clip_y_max - (1.0f * UI_SCALE_FAC));

  immRectf(pos,
           end_x - (2.0f * UI_SCALE_FAC),
           clip_y_min + (1.0f * UI_SCALE_FAC),
           end_x - (1.0f * UI_SCALE_FAC),
           clip_y_max - (1.0f * UI_SCALE_FAC));
}

static void better_timeline_draw_clip_connector(const View2D *v2d,
                                                const BetterTimelineClip *left_clip,
                                                const BetterTimelineClip *right_clip,
                                                const float clip_y_min,
                                                const float clip_y_max,
                                                const uint pos)
{
  if (left_clip == nullptr || right_clip == nullptr ||
      !ed::better_timeline::clip_types_allow_overlap(left_clip->clip_type, right_clip->clip_type))
  {
    return;
  }

  if (!STREQ(left_clip->clip_type, right_clip->clip_type)) {
    return;
  }

  UNUSED_VARS(v2d, clip_y_min, clip_y_max, pos);
}

static void better_timeline_draw_blend_overlap_at_frames(const View2D *v2d,
                                                          const BetterTimelineClip *left_clip,
                                                          const float left_start,
                                                          const float left_end,
                                                          const BetterTimelineClip *right_clip,
                                                          const float right_start,
                                                          const float right_end,
                                                          const float clip_y_min,
                                                          const float clip_y_max,
                                                          const uint pos)
{
  if (left_clip == nullptr || right_clip == nullptr ||
      !ed::better_timeline::clip_types_allow_overlap(left_clip->clip_type, right_clip->clip_type))
  {
    return;
  }

  const float overlap_start = std::max(left_start, right_start);
  const float overlap_end = std::min(left_end, right_end);
  if (overlap_end <= overlap_start) {
    return;
  }

  const float overlap_start_x = ui::view2d_view_to_region_x(v2d, overlap_start);
  const float overlap_end_x = ui::view2d_view_to_region_x(v2d, overlap_end);
  if (overlap_end_x <= overlap_start_x) {
    return;
  }

  float left_color[4];
  float right_color[4];
  better_timeline_clip_color_get(left_clip, left_color);
  better_timeline_clip_color_get(right_clip, right_color);
  left_color[3] = 0.92f;
  right_color[3] = 0.92f;

  /* Left clip (fading out): upper-left triangle of the blend zone. */
  immUniformColor4f(left_color[0], left_color[1], left_color[2], left_color[3]);
  immBegin(GPU_PRIM_TRIS, 3);
  immVertex2f(pos, overlap_start_x, clip_y_min);
  immVertex2f(pos, overlap_start_x, clip_y_max);
  immVertex2f(pos, overlap_end_x, clip_y_max);
  immEnd();

  /* Right clip (fading in): lower-right triangle of the blend zone. */
  immUniformColor4f(right_color[0], right_color[1], right_color[2], right_color[3]);
  immBegin(GPU_PRIM_TRIS, 3);
  immVertex2f(pos, overlap_start_x, clip_y_min);
  immVertex2f(pos, overlap_end_x, clip_y_max);
  immVertex2f(pos, overlap_end_x, clip_y_min);
  immEnd();

  /* Single diagonal boundary line separating the two triangles (bottom-left → top-right). */
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.35f);
  GPU_line_width(1.0f);
  immBegin(GPU_PRIM_LINES, 2);
  immVertex2f(pos, overlap_start_x, clip_y_min);
  immVertex2f(pos, overlap_end_x, clip_y_max);
  immEnd();
}

static void better_timeline_draw_blend_overlap(const View2D *v2d,
                                               const BetterTimelineClip *left_clip,
                                               const BetterTimelineClip *right_clip,
                                               const float clip_y_min,
                                               const float clip_y_max,
                                               const uint pos)
{
  better_timeline_draw_blend_overlap_at_frames(v2d,
                                               left_clip,
                                               left_clip->start_frame,
                                               left_clip->end_frame,
                                               right_clip,
                                               right_clip->start_frame,
                                               right_clip->end_frame,
                                               clip_y_min,
                                               clip_y_max,
                                               pos);
}

static void better_timeline_draw_blend_overlap_preview(const View2D *v2d,
                                                       const BetterTimelineClip *left_clip,
                                                       const float left_start_frame,
                                                       const float left_end_frame,
                                                       const BetterTimelineClip *right_clip,
                                                       const float right_start_frame,
                                                       const float right_end_frame,
                                                       const float clip_y_min,
                                                       const float clip_y_max,
                                                       const uint pos)
{
  if (left_clip == nullptr || right_clip == nullptr ||
      !ed::better_timeline::clip_types_allow_overlap(left_clip->clip_type, right_clip->clip_type))
  {
    return;
  }

  const float overlap_start = std::max(left_start_frame, right_start_frame);
  const float overlap_end = std::min(left_end_frame, right_end_frame);
  if (overlap_end <= overlap_start) {
    return;
  }

  const float overlap_start_x = ui::view2d_view_to_region_x(v2d, overlap_start);
  const float overlap_end_x = ui::view2d_view_to_region_x(v2d, overlap_end);
  if (overlap_end_x <= overlap_start_x) {
    return;
  }

  better_timeline_draw_blend_overlap_at_frames(v2d,
                                               left_clip,
                                               left_start_frame,
                                               left_end_frame,
                                               right_clip,
                                               right_start_frame,
                                               right_end_frame,
                                               clip_y_min,
                                               clip_y_max,
                                               pos);
}

static void better_timeline_draw_clip_label(const BetterTimelineClip *clip,
                                            const float start_x,
                                            const float end_x,
                                            const float clip_y_min,
                                            const float clip_y_max)
{
  const char *clip_label = clip->name[0] != '\0' ? clip->name : better_timeline_clip_type_label_get(clip);
  if (clip_label[0] == '\0' || (end_x - start_x) <= (32.0f * UI_SCALE_FAC)) {
    return;
  }

  const int font_id = BLF_default();
  const float pad_x = 4.0f * UI_SCALE_FAC;
  BLF_clipping(font_id, start_x + pad_x, clip_y_min, end_x - pad_x, clip_y_max);
  BLF_enable(font_id, BLF_CLIPPING);
  BLF_color4f(font_id, 0.97f, 0.97f, 0.97f, 0.95f);
  BLF_draw_default(start_x + (8.0f * UI_SCALE_FAC),
                   clip_y_min + ((clip_y_max - clip_y_min) * 0.5f) - (5.0f * UI_SCALE_FAC),
                   0.0f,
                   clip_label,
                   BLF_DRAW_STR_DUMMY_MAX);
  BLF_disable(font_id, BLF_CLIPPING);
}

void better_timeline_clip_drag_visual_state_update(const SpaceBetterTimeline *sbetter_timeline,
                                                   const ARegion *region,
                                                   const BetterTimelineTrack *source_track,
                                                   const BetterTimelineTrack *target_track,
                                                   const BetterTimelineClip *dragged_clip,
                                                   const Span<const BetterTimelineClip *> moved_clips,
                                                   const Span<const BetterTimelineTrack *> moved_clip_tracks,
                                                   const float preview_start_frame,
                                                   const float preview_end_frame,
                                                   const bool drop_valid)
{
  SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(sbetter_timeline);
  if (runtime == nullptr) {
    return;
  }

  BetterTimelineClipDragVisualState &state = runtime->clip_drag_visual_state;
  state.region = region;
  state.source_track = source_track;
  state.target_track = target_track;
  state.dragged_clip = dragged_clip;
  state.moved_clips.clear();
  state.moved_clips.extend(moved_clips);
  state.moved_clip_tracks.clear();
  state.moved_clip_tracks.extend(moved_clip_tracks);
  state.preview_start_frame = preview_start_frame;
  state.preview_end_frame = preview_end_frame;
  state.drop_valid = drop_valid;
  state.active = true;
}

void better_timeline_clip_drag_visual_state_clear(SpaceBetterTimeline *sbetter_timeline)
{
  SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(sbetter_timeline);
  if (runtime == nullptr) {
    return;
  }

  BetterTimelineClipDragVisualState &state = runtime->clip_drag_visual_state;
  state.region = nullptr;
  state.source_track = nullptr;
  state.target_track = nullptr;
  state.dragged_clip = nullptr;
  state.moved_clips.clear();
  state.moved_clip_tracks.clear();
  state.preview_start_frame = 0.0f;
  state.preview_end_frame = 0.0f;
  state.drop_valid = false;
  state.active = false;
}

bool better_timeline_clip_drag_visual_state_is_dragged_clip(const SpaceBetterTimeline *sbetter_timeline,
                                                            const BetterTimelineClip *clip)
{
  const SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(sbetter_timeline);
  if (runtime == nullptr) {
    return false;
  }

  const BetterTimelineClipDragVisualState &state = runtime->clip_drag_visual_state;
  return state.active && state.moved_clips.contains(clip);
}

void better_timeline_clip_box_select_visual_state_update(const SpaceBetterTimeline *sbetter_timeline,
                                                         const ARegion *region,
                                                         const rcti &rect)
{
  SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(sbetter_timeline);
  if (runtime == nullptr) {
    return;
  }

  runtime->clip_box_select_visual_state.region = region;
  runtime->clip_box_select_visual_state.rect = rect;
  runtime->clip_box_select_visual_state.active = true;
}

void better_timeline_clip_box_select_visual_state_clear(SpaceBetterTimeline *sbetter_timeline)
{
  SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(sbetter_timeline);
  if (runtime == nullptr) {
    return;
  }

  runtime->clip_box_select_visual_state.region = nullptr;
  runtime->clip_box_select_visual_state.rect = {0, 0, 0, 0};
  runtime->clip_box_select_visual_state.active = false;
}

void better_timeline_clip_resize_visual_state_update(const SpaceBetterTimeline *sbetter_timeline,
                                                     const ARegion *region,
                                                     const BetterTimelineTrack *track,
                                                     const BetterTimelineClip *clip,
                                                     const float preview_start_frame,
                                                     const float preview_end_frame)
{
  SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(
      const_cast<SpaceBetterTimeline *>(sbetter_timeline));
  if (runtime == nullptr) {
    return;
  }

  BetterTimelineClipResizeVisualState &state = runtime->clip_resize_visual_state;
  state.region = region;
  state.track = track;
  state.clip = clip;
  state.preview_start_frame = preview_start_frame;
  state.preview_end_frame = preview_end_frame;
  state.active = true;
}

void better_timeline_clip_resize_visual_state_clear(SpaceBetterTimeline *sbetter_timeline)
{
  SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(sbetter_timeline);
  if (runtime == nullptr) {
    return;
  }

  BetterTimelineClipResizeVisualState &state = runtime->clip_resize_visual_state;
  state.region = nullptr;
  state.track = nullptr;
  state.clip = nullptr;
  state.preview_start_frame = 0.0f;
  state.preview_end_frame = 0.0f;
  state.active = false;
}

bool better_timeline_clip_resize_visual_state_is_resized_clip(
    const SpaceBetterTimeline *sbetter_timeline, const BetterTimelineClip *clip)
{
  const SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(
      const_cast<SpaceBetterTimeline *>(sbetter_timeline));
  if (runtime == nullptr) {
    return false;
  }

  const BetterTimelineClipResizeVisualState &state = runtime->clip_resize_visual_state;
  return state.active && state.clip == clip;
}

static void better_timeline_draw_clip_box_select_overlay(
    const ARegion *region, const SpaceBetterTimeline *sbetter_timeline)
{
  const SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(sbetter_timeline);
  if (runtime == nullptr || !runtime->clip_box_select_visual_state.active ||
      runtime->clip_box_select_visual_state.region != region)
  {
    return;
  }

  rcti rect = runtime->clip_box_select_visual_state.rect;
  const rcti body_rect = better_timeline_body_rect(region, sbetter_timeline);
  if (!BLI_rcti_isect(&rect, &body_rect, &rect)) {
    return;
  }

  GPU_matrix_push_projection();
  wmOrtho2_region_pixelspace(region);
  GPU_blend(GPU_BLEND_ALPHA);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.05f);
  immRectf(pos, float(rect.xmin), float(rect.ymin), float(rect.xmax), float(rect.ymax));

  immUnbindProgram();

  GPU_blend(GPU_BLEND_NONE);

  const uint dashed_pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);

  float viewport_size[4];
  GPU_viewport_size_get_f(viewport_size);
  immUniform2f("viewport_size", viewport_size[2], viewport_size[3]);
  immUniform1i("colors_len", 2);
  immUniform4f("color", 0.4f, 0.4f, 0.4f, 1.0f);
  immUniform4f("color2", 1.0f, 1.0f, 1.0f, 1.0f);
  immUniform1f("dash_width", 8.0f);
  immUniform1f("udash_factor", 0.5f);
  imm_draw_box_wire_2d(
      dashed_pos, float(rect.xmin), float(rect.ymin), float(rect.xmax), float(rect.ymax));

  immUnbindProgram();
  GPU_matrix_pop_projection();
}

static void better_timeline_draw_clips(const ARegion *region,
                                       const SpaceBetterTimeline *sbetter_timeline,
                                       const View2D *v2d)
{
  const rcti body_rect = better_timeline_body_rect(region, sbetter_timeline);
  BetterTimelineClipState clip_state;
  better_timeline_clip_begin(region, body_rect, &clip_state);

  GPU_matrix_push_projection();
  wmOrtho2_region_pixelspace(region);
  GPU_blend(GPU_BLEND_ALPHA);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  const int track_count = better_timeline_track_count(sbetter_timeline);
  const SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(sbetter_timeline);
  const BetterTimelineClipDragVisualState *clip_drag_state =
      (runtime != nullptr) ? &runtime->clip_drag_visual_state : nullptr;
  const bool clip_drag_active = clip_drag_state != nullptr && clip_drag_state->active &&
                                clip_drag_state->region == region;
  const BetterTimelineClipResizeVisualState *clip_resize_state =
      (runtime != nullptr) ? &runtime->clip_resize_visual_state : nullptr;
  const bool clip_resize_active = clip_resize_state != nullptr && clip_resize_state->active &&
                                  clip_resize_state->region == region;
  for (int row_index = 0; row_index < track_count; row_index++) {
    if (!better_timeline_row_is_visible(region, sbetter_timeline, row_index)) {
      continue;
    }

    const BetterTimelineTrack *track = better_timeline_track_at_index(sbetter_timeline, row_index);
    if (track == nullptr) {
      continue;
    }

    const bool track_muted = better_timeline_track_is_muted(track);
    const bool track_locked = better_timeline_track_is_locked(track);
    const float row_y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
    const float row_y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
    const float clip_y_max = row_y_max - (6.0f * UI_SCALE_FAC);
    const float clip_y_min = row_y_min + (6.0f * UI_SCALE_FAC);

    for (const BetterTimelineClip *clip = static_cast<const BetterTimelineClip *>(track->clips.first);
         clip != nullptr;
         clip = clip->next)
    {
      if (clip_drag_active &&
          better_timeline_clip_drag_visual_state_is_dragged_clip(sbetter_timeline, clip))
      {
        continue;
      }
      if (clip_resize_active &&
          better_timeline_clip_resize_visual_state_is_resized_clip(sbetter_timeline, clip))
      {
        continue;
      }

      float start_x = ui::view2d_view_to_region_x(v2d, clip->start_frame);
      float end_x = ui::view2d_view_to_region_x(v2d, clip->end_frame);
      if (end_x < body_rect.xmin || start_x > body_rect.xmax) {
        continue;
      }

      if (end_x <= start_x) {
        end_x = start_x + 1.0f;
      }

      end_x = std::max(end_x, start_x + (10.0f * UI_SCALE_FAC));
      float snapped_clip_y_min = clip_y_min;
      float snapped_clip_y_max = clip_y_max;
      better_timeline_clip_rect_pixel_snap(
          &start_x, &end_x, &snapped_clip_y_min, &snapped_clip_y_max);

      float clip_color[4];
      better_timeline_clip_color_get(clip, clip_color);
      if (track_muted) {
        /* Desaturate to a flat grey to signal the track is muted. */
        const float lum = clip_color[0] * 0.2126f + clip_color[1] * 0.7152f +
                          clip_color[2] * 0.0722f;
        const float grey = lum * 0.5f + 0.22f;
        clip_color[0] = grey;
        clip_color[1] = grey;
        clip_color[2] = grey;
        clip_color[3] *= 0.55f;
      }
      if (track_locked) {
        /* Desaturate to grey to signal the track is locked (clips cannot be edited). */
        const float lum = clip_color[0] * 0.2126f + clip_color[1] * 0.7152f +
                          clip_color[2] * 0.0722f;
        const float grey = lum * 0.5f + 0.22f;
        clip_color[0] = grey;
        clip_color[1] = grey;
        clip_color[2] = grey;
        clip_color[3] *= 0.70f;
      }
      immUniformColor4f(clip_color[0], clip_color[1], clip_color[2], clip_color[3]);
      immRectf(pos, start_x, snapped_clip_y_min, end_x, snapped_clip_y_max);

      better_timeline_draw_clip_bottom_accent(
          start_x, end_x, snapped_clip_y_min, pos, clip_color);

      if (better_timeline_clip_is_selected(clip)) {
        better_timeline_draw_clip_selection_highlight(
            start_x, end_x, snapped_clip_y_min, snapped_clip_y_max, pos);
      }

      float outline_color[4];
      better_timeline_clip_outline_color_get(clip, outline_color);
      better_timeline_draw_clip_outline(start_x,
                                        end_x,
                                        snapped_clip_y_min,
                                        snapped_clip_y_max,
                                        pos,
                                        outline_color,
                                        better_timeline_clip_is_selected(clip));
      better_timeline_draw_clip_label(
          clip, start_x, end_x, snapped_clip_y_min, snapped_clip_y_max);

      /* Resize grip lines: subtle bright vertical markers at each handle zone boundary
       * on selected clips so the user knows where to grab for resize. */
      if (better_timeline_clip_is_selected(clip)) {
        const float handle_w = BETTER_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH * UI_SCALE_FAC;
        const float inset_y = 4.0f * UI_SCALE_FAC;
        immUniformColor4f(1.0f, 1.0f, 1.0f, 0.50f);
        immRectf(pos,
                 start_x + handle_w - 1.0f,
                 snapped_clip_y_min + inset_y,
                 start_x + handle_w,
                 snapped_clip_y_max - inset_y);
        immRectf(pos,
                 end_x - handle_w,
                 snapped_clip_y_min + inset_y,
                 end_x - handle_w + 1.0f,
                 snapped_clip_y_max - inset_y);
      }
    }

    for (const BetterTimelineClip *clip = static_cast<const BetterTimelineClip *>(track->clips.first);
         clip != nullptr;
         clip = clip->next)
    {
      if (clip_drag_active &&
          better_timeline_clip_drag_visual_state_is_dragged_clip(sbetter_timeline, clip))
      {
        continue;
      }
      if (clip_resize_active &&
          better_timeline_clip_resize_visual_state_is_resized_clip(sbetter_timeline, clip))
      {
        continue;
      }

      for (const BetterTimelineClip *other_clip = clip->next; other_clip != nullptr;
           other_clip = other_clip->next)
      {
        if (clip_drag_active &&
            better_timeline_clip_drag_visual_state_is_dragged_clip(sbetter_timeline, other_clip))
        {
          continue;
        }
        if (clip_resize_active &&
            better_timeline_clip_resize_visual_state_is_resized_clip(sbetter_timeline, other_clip))
        {
          continue;
        }

        better_timeline_draw_clip_connector(
            v2d, clip, other_clip, clip_y_min, clip_y_max, pos);
        better_timeline_draw_blend_overlap(
            v2d, clip, other_clip, clip_y_min, clip_y_max, pos);
      }
    }
  }

  if (clip_drag_active && clip_drag_state->dragged_clip != nullptr) {
    const float frame_delta = clip_drag_state->preview_start_frame -
                              clip_drag_state->dragged_clip->start_frame;
    for (int i = 0; i < clip_drag_state->moved_clips.size(); i++) {
      const BetterTimelineClip *clip = clip_drag_state->moved_clips[i];
      const BetterTimelineTrack *preview_track = clip_drag_state->moved_clip_tracks[i];
      const int row_index = better_timeline_track_index_from_ptr(sbetter_timeline, preview_track);
      if (row_index < 0) {
        continue;
      }

      const float row_y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
      const float row_y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
      const float clip_y_max = row_y_max - (6.0f * UI_SCALE_FAC);
      const float clip_y_min = row_y_min + (6.0f * UI_SCALE_FAC);

      float start_x = ui::view2d_view_to_region_x(v2d, clip->start_frame + frame_delta);
      float end_x = ui::view2d_view_to_region_x(v2d, clip->end_frame + frame_delta);
      end_x = std::max(end_x, start_x + (10.0f * UI_SCALE_FAC));
      float snapped_clip_y_min = clip_y_min;
      float snapped_clip_y_max = clip_y_max;
      better_timeline_clip_rect_pixel_snap(
          &start_x, &end_x, &snapped_clip_y_min, &snapped_clip_y_max);

      float clip_color[4];
      better_timeline_clip_color_get(clip, clip_color);
      if (!clip_drag_state->drop_valid) {
        clip_color[0] = 0.86f;
        clip_color[1] = 0.24f;
        clip_color[2] = 0.24f;
      }
      clip_color[3] = 0.92f;

      immUniformColor4f(clip_color[0], clip_color[1], clip_color[2], clip_color[3]);
      immRectf(pos, start_x, snapped_clip_y_min, end_x, snapped_clip_y_max);
      better_timeline_draw_clip_bottom_accent(
          start_x, end_x, snapped_clip_y_min, pos, clip_color);
      if (better_timeline_clip_is_selected(clip)) {
        better_timeline_draw_clip_selection_highlight(
            start_x, end_x, snapped_clip_y_min, snapped_clip_y_max, pos);
      }
      float outline_color[4];
      better_timeline_clip_outline_color_get(clip, outline_color);
      better_timeline_draw_clip_outline(
          start_x,
          end_x,
          snapped_clip_y_min,
          snapped_clip_y_max,
          pos,
          outline_color,
          better_timeline_clip_is_selected(clip));
      better_timeline_draw_clip_label(
          clip, start_x, end_x, snapped_clip_y_min, snapped_clip_y_max);
    }

    for (int i = 0; i < clip_drag_state->moved_clips.size(); i++) {
      const BetterTimelineClip *preview_clip = clip_drag_state->moved_clips[i];
      const BetterTimelineTrack *preview_track = clip_drag_state->moved_clip_tracks[i];
      const int row_index = better_timeline_track_index_from_ptr(sbetter_timeline, preview_track);
      if (row_index < 0) {
        continue;
      }

      const float row_y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
      const float row_y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
      const float clip_y_max = row_y_max - (6.0f * UI_SCALE_FAC);
      const float clip_y_min = row_y_min + (6.0f * UI_SCALE_FAC);
      const float preview_start_frame = preview_clip->start_frame + frame_delta;
      const float preview_end_frame = preview_clip->end_frame + frame_delta;

      for (const BetterTimelineClip *other_clip = static_cast<const BetterTimelineClip *>(preview_track->clips.first);
           other_clip != nullptr;
           other_clip = other_clip->next)
      {
        if (better_timeline_clip_drag_visual_state_is_dragged_clip(sbetter_timeline, other_clip)) {
          continue;
        }

        better_timeline_draw_blend_overlap_preview(v2d,
                                                   preview_clip,
                                                   preview_start_frame,
                                                   preview_end_frame,
                                                   other_clip,
                                                   other_clip->start_frame,
                                                   other_clip->end_frame,
                                                   clip_y_min,
                                                   clip_y_max,
                                                   pos);
      }

      for (int j = i + 1; j < clip_drag_state->moved_clips.size(); j++) {
        const BetterTimelineClip *other_preview_clip = clip_drag_state->moved_clips[j];
        const BetterTimelineTrack *other_preview_track = clip_drag_state->moved_clip_tracks[j];
        if (preview_track != other_preview_track) {
          continue;
        }

        better_timeline_draw_blend_overlap_preview(
            v2d,
            preview_clip,
            preview_start_frame,
            preview_end_frame,
            other_preview_clip,
            other_preview_clip->start_frame + frame_delta,
            other_preview_clip->end_frame + frame_delta,
            clip_y_min,
            clip_y_max,
            pos);
      }
    }
  }

  /* Resize preview: draw the resized clip at its preview bounds. */
  if (clip_resize_active && clip_resize_state->clip != nullptr &&
      clip_resize_state->track != nullptr)
  {
    const BetterTimelineClip *clip = clip_resize_state->clip;
    const BetterTimelineTrack *preview_track = clip_resize_state->track;
    const int row_index = better_timeline_track_index_from_ptr(sbetter_timeline, preview_track);
    if (row_index >= 0) {
      const float row_y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
      const float row_y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
      const float clip_y_max = row_y_max - (6.0f * UI_SCALE_FAC);
      const float clip_y_min = row_y_min + (6.0f * UI_SCALE_FAC);

      float start_x = ui::view2d_view_to_region_x(v2d, clip_resize_state->preview_start_frame);
      float end_x = ui::view2d_view_to_region_x(v2d, clip_resize_state->preview_end_frame);
      end_x = std::max(end_x, start_x + (10.0f * UI_SCALE_FAC));
      float snapped_clip_y_min = clip_y_min;
      float snapped_clip_y_max = clip_y_max;
      better_timeline_clip_rect_pixel_snap(
          &start_x, &end_x, &snapped_clip_y_min, &snapped_clip_y_max);

      float clip_color[4];
      better_timeline_clip_color_get(clip, clip_color);
      clip_color[3] = 0.92f;
      immUniformColor4f(clip_color[0], clip_color[1], clip_color[2], clip_color[3]);
      immRectf(pos, start_x, snapped_clip_y_min, end_x, snapped_clip_y_max);

      better_timeline_draw_clip_bottom_accent(
          start_x, end_x, snapped_clip_y_min, pos, clip_color);
      better_timeline_draw_clip_selection_highlight(
          start_x, end_x, snapped_clip_y_min, snapped_clip_y_max, pos);

      float outline_color[4];
      better_timeline_clip_outline_color_get(clip, outline_color);
      better_timeline_draw_clip_outline(start_x,
                                        end_x,
                                        snapped_clip_y_min,
                                        snapped_clip_y_max,
                                        pos,
                                        outline_color,
                                        true);
      better_timeline_draw_clip_label(
          clip, start_x, end_x, snapped_clip_y_min, snapped_clip_y_max);

      /* Resize grip lines on preview clip, matching the static selected-clip indicator. */
      const float handle_w = BETTER_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH * UI_SCALE_FAC;
      const float inset_y = 4.0f * UI_SCALE_FAC;
      immUniformColor4f(1.0f, 1.0f, 1.0f, 0.50f);
      immRectf(pos,
               start_x + handle_w - 1.0f,
               snapped_clip_y_min + inset_y,
               start_x + handle_w,
               snapped_clip_y_max - inset_y);
      immRectf(pos,
               end_x - handle_w,
               snapped_clip_y_min + inset_y,
               end_x - handle_w + 1.0f,
               snapped_clip_y_max - inset_y);

      /* Draw blend overlaps between the resize preview bounds and every other clip in the track.
       * This ensures the blend zone remains visible (and correctly positioned) while dragging. */
      for (const BetterTimelineClip *other_clip =
               static_cast<const BetterTimelineClip *>(preview_track->clips.first);
           other_clip != nullptr;
           other_clip = other_clip->next)
      {
        if (other_clip == clip) {
          /* Skip the clip being resized — its preview bounds are handled below. */
          continue;
        }

        better_timeline_draw_blend_overlap_preview(v2d,
                                                   clip,
                                                   clip_resize_state->preview_start_frame,
                                                   clip_resize_state->preview_end_frame,
                                                   other_clip,
                                                   other_clip->start_frame,
                                                   other_clip->end_frame,
                                                   snapped_clip_y_min,
                                                   snapped_clip_y_max,
                                                   pos);
      }
    }
  }

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_matrix_pop_projection();
  better_timeline_clip_end(clip_state);
}

void better_timeline_track_drag_visual_state_update(const SpaceBetterTimeline *sbetter_timeline,
                                                    const ARegion *region,
                                                    const BetterTimelineTrack *dragged_track,
                                                    const int insertion_index)
{
  SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(sbetter_timeline);
  if (runtime == nullptr) {
    return;
  }
  runtime->track_drag_visual_state.region = region;
  runtime->track_drag_visual_state.dragged_track = dragged_track;
  runtime->track_drag_visual_state.insertion_index = insertion_index;
  runtime->track_drag_visual_state.active = true;
}

void better_timeline_track_drag_visual_state_clear(SpaceBetterTimeline *sbetter_timeline)
{
  SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(sbetter_timeline);
  if (runtime == nullptr) {
    return;
  }
  runtime->track_drag_visual_state.region = nullptr;
  runtime->track_drag_visual_state.dragged_track = nullptr;
  runtime->track_drag_visual_state.insertion_index = -1;
  runtime->track_drag_visual_state.active = false;
}

static void better_timeline_draw_layout_overlay(const ARegion *region,
                                                const SpaceBetterTimeline *sbetter_timeline)
{
  const int left_panel_width = better_timeline_left_panel_width(region, sbetter_timeline);
  const int content_top = better_timeline_content_height(region);
  const int track_count = better_timeline_track_count(sbetter_timeline);
  const SpaceBetterTimeline_Runtime *runtime = better_timeline_runtime_get(sbetter_timeline);
  const BetterTimelineTrackDragVisualState *track_drag_state =
      (runtime != nullptr) ? &runtime->track_drag_visual_state : nullptr;
  const bool drag_active = track_drag_state != nullptr && track_drag_state->active &&
                           track_drag_state->region == region;
  const BetterTimelineTrack *dragged_track = drag_active ? track_drag_state->dragged_track :
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
    const bool track_muted = better_timeline_track_is_muted(track);
    const bool track_locked = better_timeline_track_is_locked(track);
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

    /* Muted and locked share the same darkening overlay. Keep it single-pass so mute+lock does
     * not stack twice, but make the single pass stronger than the old per-state overlay. */
    if (track_muted || track_locked) {
      immUniformColor4f(0.0f, 0.0f, 0.0f, 0.56f);
      immRectf(pos, 0.0f, y_min, float(left_panel_width), y_max);
      immUniformColor4f(0.0f, 0.0f, 0.0f, 0.44f);
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
        region, sbetter_timeline, track_drag_state->insertion_index);

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

  /* Draw the accent line at the *bottom* of the scrub ruler (above the track rows) so it
   * does not intrude into row 0 and cause a visual offset for the first track. */
  immUniformColor4f(0.29f, 0.58f, 0.96f, 0.9f);
  immRectf(pos,
           float(left_panel_width),
           float(content_top),
           float(region->winx),
           float(content_top + 2));

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

  /* --- Diagonal stripe pass: muted and locked tracks --- */
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_width(1.0f);
  better_timeline_clip_begin(region, content_rect, &content_clip_state);
  {
    GPUVertFormat *fmt_s = immVertexFormat();
    const uint pos_s = GPU_vertformat_attr_add(fmt_s, "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor4f(1.0f, 1.0f, 1.0f, 0.045f);
    for (int row_index = 0; row_index < track_count; row_index++) {
      if (!better_timeline_row_is_visible(region, sbetter_timeline, row_index)) {
        continue;
      }
      const BetterTimelineTrack *track_s = better_timeline_track_at_index(sbetter_timeline,
                                                                          row_index);
      if (!better_timeline_track_is_locked(track_s)) {
        continue;
      }
      const float ys_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
      const float ys_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
      better_timeline_draw_diagonal_stripes(
          0.0f, ys_min, float(region->winx), ys_max, pos_s);
    }
    immUnbindProgram();
  }
  better_timeline_clip_end(content_clip_state);
  GPU_blend(GPU_BLEND_NONE);

  /* --- Accent bars and button backgrounds (geometry pass addition) --- */
  GPU_blend(GPU_BLEND_ALPHA);
  {
    GPUVertFormat *fmt2 = immVertexFormat();
    const uint pos2 = GPU_vertformat_attr_add(fmt2, "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

    better_timeline_clip_begin(region, content_rect, &content_clip_state);
    for (int row_index = 0; row_index < track_count; row_index++) {
      if (!better_timeline_row_is_visible(region, sbetter_timeline, row_index)) {
        continue;
      }
      const BetterTimelineTrack *track = better_timeline_track_at_index(
          sbetter_timeline, row_index);
      if (track == nullptr) {
        continue;
      }
      const float y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
      const float y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
      const float accent_w = float(BETTER_TIMELINE_TRACK_ACCENT_WIDTH) * UI_SCALE_FAC;

      /* Left accent bar: coloured stripe representing the track type. */
      float accent_color[3] = {0.5f, 0.5f, 0.5f};
      const ed::better_timeline::BetterTimelineTrackType *tt =
          ed::better_timeline::track_type_find_from_idname(track->track_type);
      if (tt != nullptr) {
        accent_color[0] = tt->color[0];
        accent_color[1] = tt->color[1];
        accent_color[2] = tt->color[2];
      }
      const float accent_alpha = better_timeline_track_is_muted(track) ? 0.35f : 0.88f;
      const float accent_pad = 1.0f * UI_SCALE_FAC;
      immUniformColor4f(accent_color[0], accent_color[1], accent_color[2], accent_alpha);
      immRectf(pos2, accent_pad, y_min + accent_pad, accent_w, y_max - accent_pad);

      /* Subtle button background for mute/lock button zone. */
      const rcti mute_rect = better_timeline_track_mute_button_rect(
          region, sbetter_timeline, row_index);
      const rcti lock_rect = better_timeline_track_lock_button_rect(
          region, sbetter_timeline, row_index);
      const float btn_bg_alpha = 0.12f;
      immUniformColor4f(1.0f, 1.0f, 1.0f, btn_bg_alpha);
      immRectf(pos2,
               float(mute_rect.xmin),
               float(mute_rect.ymin),
               float(mute_rect.xmax),
               float(mute_rect.ymax));
      immRectf(pos2,
               float(lock_rect.xmin),
               float(lock_rect.ymin),
               float(lock_rect.xmax),
               float(lock_rect.ymax));
    }
    better_timeline_clip_end(content_clip_state);
    immUnbindProgram();
  }
  GPU_blend(GPU_BLEND_NONE);

  /* --- Track name text pass --- */
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
    if (better_timeline_track_is_muted(track)) {
      text_color[3] = uchar(text_color[3] * 0.55f);
    }
    BLF_color4ubv(BLF_default(), text_color);

    /* Name starts after: accent bar + gap + icon area + gap. */
    const float accent_w = float(BETTER_TIMELINE_TRACK_ACCENT_WIDTH) * UI_SCALE_FAC;
    const float icon_area = float(BETTER_TIMELINE_TRACK_BUTTON_SIZE) * UI_SCALE_FAC;
    const float name_x = accent_w + (4.0f * UI_SCALE_FAC) + icon_area + (4.0f * UI_SCALE_FAC);
    const float y = better_timeline_row_ymin(region, sbetter_timeline, row_index) +
                    (BETTER_TIMELINE_ROW_HEIGHT * 0.5f) - (5.0f * UI_SCALE_FAC);
    BLF_draw_default(name_x, y, 0.0f, track->name, BLF_DRAW_STR_DUMMY_MAX);
  }
  better_timeline_clip_end(content_clip_state);

  /* --- Icon pass: track type icon + mute/lock button icons --- */
  GPU_blend(GPU_BLEND_ALPHA);
  for (int row_index = 0; row_index < track_count; row_index++) {
    if (!better_timeline_row_is_visible(region, sbetter_timeline, row_index)) {
      continue;
    }
    const BetterTimelineTrack *track = better_timeline_track_at_index(sbetter_timeline, row_index);
    if (track == nullptr) {
      continue;
    }
    const float y_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
    const float y_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
    const float row_center_y = (y_min + y_max) * 0.5f;
    const float icon_size = float(UI_ICON_SIZE);
    const float accent_w = float(BETTER_TIMELINE_TRACK_ACCENT_WIDTH) * UI_SCALE_FAC;
    uchar icon_color[4];
    ui::theme::get_color_4ubv(better_timeline_track_is_selected(track) ? TH_HEADER_TEXT_HI :
                                                                           TH_TEXT,
                              icon_color);
    if (better_timeline_track_is_muted(track)) {
      icon_color[3] = uchar(icon_color[3] * 0.78f);
    }

    /* Track type icon. */
    const ed::better_timeline::BetterTimelineTrackType *tt =
        ed::better_timeline::track_type_find_from_idname(track->track_type);
    const int type_icon = (tt != nullptr) ? tt->icon : ICON_SEQUENCE;
    const float type_icon_x = accent_w + (2.0f * UI_SCALE_FAC);
    const float type_icon_y = row_center_y - icon_size * 0.5f;
    const float type_icon_alpha = better_timeline_track_is_muted(track) ? 0.72f : 0.92f;
    ui::icon_draw_ex(type_icon_x,
                     type_icon_y,
                     type_icon,
                     1.0f / UI_SCALE_FAC,
                     type_icon_alpha,
                     0.0f,
                     icon_color,
                     false,
                     nullptr);

    /* Mute (eye) button icon. */
    const int mute_icon = better_timeline_track_is_muted(track) ? ICON_HIDE_ON : ICON_HIDE_OFF;
    const rcti mute_rect = better_timeline_track_mute_button_rect(
        region, sbetter_timeline, row_index);
    const float mute_icon_x = mute_rect.xmin +
                               (BLI_rcti_size_x(&mute_rect) - icon_size) * 0.5f;
    const float mute_icon_y = row_center_y - icon_size * 0.5f;
    const float mute_icon_alpha = better_timeline_track_is_muted(track) ? 0.92f : 0.78f;
    ui::icon_draw_ex(mute_icon_x,
                     mute_icon_y,
                     mute_icon,
                     1.0f / UI_SCALE_FAC,
                     mute_icon_alpha,
                     0.0f,
                     icon_color,
                     false,
                     nullptr);

    /* Lock button icon. */
    const int lock_icon = better_timeline_track_is_locked(track) ? ICON_LOCKED : ICON_UNLOCKED;
    const rcti lock_rect = better_timeline_track_lock_button_rect(
        region, sbetter_timeline, row_index);
    const float lock_icon_x = lock_rect.xmin +
                               (BLI_rcti_size_x(&lock_rect) - icon_size) * 0.5f;
    const float lock_icon_y = row_center_y - icon_size * 0.5f;
    const float lock_icon_alpha = better_timeline_track_is_locked(track) ? 0.96f : 0.78f;
    ui::icon_draw_ex(lock_icon_x,
                     lock_icon_y,
                     lock_icon,
                     1.0f / UI_SCALE_FAC,
                     lock_icon_alpha,
                     0.0f,
                     icon_color,
                     false,
                     nullptr);
  }
  GPU_blend(GPU_BLEND_NONE);

  /* --- "Locked" / "Muted" status label in the timeline canvas --- */
  {
    const rcti body_rect_lbl = better_timeline_body_rect(region, sbetter_timeline);
    const float canvas_center_x = (float(body_rect_lbl.xmin) + float(body_rect_lbl.xmax)) * 0.5f;
    const float pad_x = 9.0f * UI_SCALE_FAC;
    const float pad_y = 5.0f * UI_SCALE_FAC;
    const float box_radius = 3.0f * UI_SCALE_FAC;
    const float edge_safe_inset = 2.0f * UI_SCALE_FAC;
    const uiStyle *style = ui::style_get_dpi();
    const uiFontStyle *label_font = &style->widget;
    const ui::FontStyleDrawParams label_draw_params{ui::UI_STYLE_TEXT_CENTER, 0};
    const uchar label_text_color[4] = {217, 217, 217, 235};

    ui::fontstyle_set(label_font);

    /* Pre-compute uniform box size — take the max of both strings so every
     * box is the same width/height regardless of which label is shown. */
    const float max_lbl_w = std::max(
        std::max(BLF_width(label_font->uifont_id, "Muted", BLF_DRAW_STR_DUMMY_MAX),
                 BLF_width(label_font->uifont_id, "Locked", BLF_DRAW_STR_DUMMY_MAX)),
        BLF_width(label_font->uifont_id, "Locked / Muted", BLF_DRAW_STR_DUMMY_MAX));
    const float max_lbl_h = std::max(
        float(BLF_height_max(label_font->uifont_id)),
        float(BLF_height_max(label_font->uifont_id)));
    const float box_w = max_lbl_w + pad_x * 2.0f;
    const float box_h = max_lbl_h + pad_y * 2.0f;
    /* Box x-coords are the same for every row. */
    const float bx0 = canvas_center_x - box_w * 0.5f;
    const float bx1 = bx0 + box_w;

    BetterTimelineClipState lbl_clip_state;
    better_timeline_clip_begin(region, body_rect_lbl, &lbl_clip_state);
    GPU_blend(GPU_BLEND_ALPHA);

    ui::draw_roundbox_corner_set(ui::CNR_ALL);

    for (int row_index = 0; row_index < track_count; row_index++) {
      if (!better_timeline_row_is_visible(region, sbetter_timeline, row_index)) {
        continue;
      }
      const BetterTimelineTrack *track_l = better_timeline_track_at_index(sbetter_timeline,
                                                                          row_index);
      const bool lbl_muted = better_timeline_track_is_muted(track_l);
      const bool lbl_locked = better_timeline_track_is_locked(track_l);
      if (!lbl_muted && !lbl_locked) {
        continue;
      }
      const char *lbl_text = (lbl_locked && lbl_muted) ? "Locked / Muted" :
                             lbl_locked                 ? "Locked" :
                                                          "Muted";
      const float yl_min = better_timeline_row_ymin(region, sbetter_timeline, row_index);
      const float yl_max = better_timeline_row_ymax(region, sbetter_timeline, row_index);
      const float visible_row_min = std::max(yl_min, float(body_rect_lbl.ymin));
      const float visible_row_max = std::min(yl_max, float(body_rect_lbl.ymax));
      if (visible_row_max <= visible_row_min) {
        continue;
      }
      const float row_cy = (visible_row_min + visible_row_max) * 0.5f;
      float by0 = row_cy - box_h * 0.5f;
      float by1 = by0 + box_h;
      const float label_y_min = float(body_rect_lbl.ymin) + edge_safe_inset;
      const float label_y_max = float(body_rect_lbl.ymax) - edge_safe_inset;
      if (by1 > label_y_max) {
        const float shift = by1 - label_y_max;
        by0 -= shift;
        by1 -= shift;
      }
      if (by0 < label_y_min) {
        const float shift = label_y_min - by0;
        by0 += shift;
        by1 += shift;
      }

      /* Rounded background. */
      const rctf box_rctf = {bx0, bx1, by0, by1};
      const float bg_col[4] = {0.12f, 0.12f, 0.12f, 0.88f};
      ui::draw_roundbox_4fv(&box_rctf, true, box_radius, bg_col);

      /* Rounded border. */
      const float border_col[4] = {0.50f, 0.50f, 0.50f, 0.80f};
      ui::draw_roundbox_4fv(&box_rctf, false, box_radius, border_col);

      const rcti text_rect = {int(std::round(bx0)),
                              int(std::round(bx1)),
                              int(std::round(by0)),
                              int(std::round(by1))};
      ui::fontstyle_draw(label_font,
                         &text_rect,
                         lbl_text,
                         BLF_DRAW_STR_DUMMY_MAX,
                         label_text_color,
                         &label_draw_params);
    }

    GPU_blend(GPU_BLEND_NONE);
    better_timeline_clip_end(lbl_clip_state);
  }

  GPU_matrix_pop_projection();
}

void better_timeline_main_region_draw(const bContext *C, ARegion *region)
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
  ui::view2d_view_restore(C);
  better_timeline_clip_end(clip_state);
  better_timeline_draw_clips(region, sbetter_timeline, v2d);
  if (current_frame_x >= body_rect.xmin && current_frame_x <= body_rect.xmax) {
    better_timeline_clip_begin(region, body_rect, &clip_state);
    better_timeline_view_ortho(v2d);
    ANIM_draw_cfra(C, v2d, DRAWCFRA_WIDE);
    ui::view2d_view_restore(C);
    better_timeline_clip_end(clip_state);
  }

  const rcti scrub_rect = better_timeline_scrub_rect(region, sbetter_timeline);
  better_timeline_clip_begin(region, scrub_rect, &clip_state);
  ED_time_scrub_draw(region, scene, false, true, round_db_to_int(scene->frames_per_second()));
  better_timeline_clip_end(clip_state);
  better_timeline_draw_layout_overlay(region, sbetter_timeline);
}

void better_timeline_main_region_draw_overlay(const bContext *C, ARegion *region)
{
  const Scene *scene = CTX_data_scene(C);
  if (scene == nullptr) {
    return;
  }

  /* Keep the scrub overlay in sync with the latest region dimensions during live resize. */
  const auto *sbetter_timeline = static_cast<const SpaceBetterTimeline *>(
      CTX_wm_area(C)->spacedata.first);
  better_timeline_view_sync(region, scene, sbetter_timeline);
  const rcti scrub_rect = better_timeline_scrub_rect(region, sbetter_timeline);
  const float current_frame_x = ui::view2d_view_to_region_x(&region->v2d, BKE_scene_ctime_get(scene));
  BetterTimelineClipState clip_state;
  if (current_frame_x >= scrub_rect.xmin && current_frame_x <= scrub_rect.xmax) {
    better_timeline_clip_begin(region, scrub_rect, &clip_state);
    ED_time_scrub_draw_current_frame(region, scene, false, false);
    better_timeline_clip_end(clip_state);
  }
  better_timeline_draw_clip_box_select_overlay(region, sbetter_timeline);
}

void better_timeline_main_region_listener(const wmRegionListenerParams *params)
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

}  // namespace blender
