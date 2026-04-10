# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  ../include
  ../../makesrna

  # RNA_prototypes.hh
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
)

set(INC_SYS
)

set(SRC
  better_timeline_data.cc
  better_timeline_draw.cc
  better_timeline_layout.cc
  better_timeline_ops_clips.cc
  better_timeline_ops_tracks.cc
  better_timeline_ops_view.cc
  better_timeline_types.cc
  space_better_timeline.cc

  better_timeline_intern.hh
)

set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenfont
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::gpu
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::windowmanager
)

blender_add_lib(bf_editor_space_better_timeline "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

# RNA_prototypes.hh dna_type_offsets.h
add_dependencies(bf_editor_space_better_timeline bf_rna)
