# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import Header, Menu, Panel

from bpy.app.translations import contexts as i18n_contexts

from bl_ui.space_time import marker_menu_generic


def better_timeline_playback_controls(layout, context):
    scene = context.scene
    screen = context.screen
    tool_settings = context.tool_settings

    if not scene:
        return

    layout.separator_spacer()

    row = layout.row(align=True)
    row.operator("screen.frame_jump", text="", icon='REW').end = False

    if not screen.is_animation_playing:
        if scene.sync_mode == 'AUDIO_SYNC' and context.preferences.system.audio_device == 'JACK':
            row.scale_x = 2
            row.operator("screen.animation_play", text="", icon='PLAY')
            row.scale_x = 1
        else:
            row.operator("screen.animation_play", text="", icon='PLAY_REVERSE').reverse = True
            row.operator("screen.animation_play", text="", icon='PLAY')
    else:
        row.scale_x = 2
        row.operator("screen.animation_play", text="", icon='PAUSE')
        row.scale_x = 1

    row.operator("screen.frame_jump", text="", icon='FF').end = True

    row = layout.row(align=True)
    row.operator("screen.time_jump", text="", icon='FRAME_PREV').backward = True
    row.operator("screen.time_jump", text="", icon='FRAME_NEXT').backward = False
    row.popover(panel="BETTER_TIMELINE_PT_jump", text="")

    row = layout.row(align=True)
    row.prop(tool_settings, "use_snap_playhead", text="")
    sub = row.row(align=True)
    sub.popover(panel="BETTER_TIMELINE_PT_playhead_snapping", text="")

    layout.separator_spacer()

    row = layout.row()
    if scene.show_subframe:
        row.scale_x = 1.15
        row.prop(scene, "frame_float", text="")
    else:
        row.scale_x = 0.95
        row.prop(scene, "frame_current", text="")

    row = layout.row(align=True)
    row.prop(scene, "use_preview_range", text="", toggle=True)
    sub = row.row(align=True)
    sub.scale_x = 0.8
    if not scene.use_preview_range:
        sub.prop(scene, "frame_start", text="Start")
        sub.prop(scene, "frame_end", text="End")
    else:
        sub.prop(scene, "frame_preview_start", text="Start")
        sub.prop(scene, "frame_preview_end", text="End")


def draw_better_timeline_playback_settings(layout, context):
    scene = context.scene
    screen = context.screen

    layout.prop(scene, "sync_mode", text="Sync")
    layout.separator()

    col = layout.column(heading="Audio")
    col.prop(scene, "use_audio_scrub", text="Scrubbing")
    col.prop(scene, "use_audio", text="Playback")

    col = layout.column(heading="Playback")
    col.prop(scene, "lock_frame_selection_to_range", text="Limit to Frame Range")
    col.prop(screen, "use_follow", text="Follow Current Frame")

    col = layout.column(heading="Play In")
    col.prop(screen, "use_play_top_left_3d_editor", text="Active Editor")
    col.prop(screen, "use_play_3d_editors", text="3D Viewport")
    col.prop(screen, "use_play_animation_editors", text="Animation Editors")
    col.prop(screen, "use_play_image_editors", text="Image Editor")
    col.prop(screen, "use_play_properties_editors", text="Properties and Sidebars")
    col.prop(screen, "use_play_clip_editors", text="Movie Clip Editor")
    col.prop(screen, "use_play_node_editors", text="Node Editors")
    col.prop(screen, "use_play_sequence_editors", text="Video Sequencer")
    col.prop(screen, "use_play_spreadsheet_editors", text="Spreadsheet")

    col = layout.column(heading="Show")
    col.prop(scene, "show_subframe", text="Subframes")

    layout.separator()

    row = layout.row(align=True)
    row.operator("anim.start_frame_set")
    row.operator("anim.end_frame_set")


class BetterTimelineButtonsPanel:
    bl_space_type = 'BETTER_TIMELINE'
    bl_region_type = 'HEADER'


class BetterTimelineSidebarPanel:
    bl_space_type = 'BETTER_TIMELINE'
    bl_region_type = 'UI'
    bl_category = "Item"


class BETTER_TIMELINE_HT_header(Header):
    bl_space_type = 'BETTER_TIMELINE'

    def draw(self, context):
        layout = self.layout

        layout.template_header()
        BETTER_TIMELINE_MT_editor_menus.draw_collapsible(context, layout)
        better_timeline_playback_controls(layout, context)


class BETTER_TIMELINE_MT_editor_menus(Menu):
    bl_idname = "BETTER_TIMELINE_MT_editor_menus"
    bl_label = ""

    def draw(self, context):
        layout = self.layout
        layout.menu("BETTER_TIMELINE_MT_view")
        layout.menu("BETTER_TIMELINE_MT_marker")
        layout.menu("BETTER_TIMELINE_MT_playback")


class BETTER_TIMELINE_MT_view(Menu):
    bl_label = "View"

    def draw(self, context):
        layout = self.layout
        scene = context.scene

        if scene.use_preview_range:
            layout.operator("anim.scene_range_frame", text="Frame Preview Range")
        else:
            layout.operator("anim.scene_range_frame", text="Frame Scene Range")

        layout.separator()
        layout.prop(scene, "show_subframe", text="Show Subframes")
        layout.menu("INFO_MT_area")


class BETTER_TIMELINE_MT_marker(Menu):
    bl_label = "Marker"

    def draw(self, context):
        marker_menu_generic(self.layout, context)


class BETTER_TIMELINE_MT_playback(Menu):
    bl_label = "Playback"
    bl_translation_context = i18n_contexts.id_windowmanager

    def draw(self, context):
        draw_better_timeline_playback_settings(self.layout, context)


class BETTER_TIMELINE_PT_playhead_snapping(BetterTimelineButtonsPanel, Panel):
    bl_label = "Playhead"
    bl_ui_units_x = 12

    def draw(self, context):
        tool_settings = context.tool_settings
        layout = self.layout
        col = layout.column()

        col.prop(tool_settings, "playhead_snap_distance")
        col.separator()
        col.label(text="Snap Target")
        col.prop(tool_settings, "snap_playhead_element", expand=True)
        col.separator()

        if 'FRAME' in tool_settings.snap_playhead_element:
            col.prop(tool_settings, "snap_playhead_frame_step")
        if 'SECOND' in tool_settings.snap_playhead_element:
            col.prop(tool_settings, "snap_playhead_second_step")


class BETTER_TIMELINE_PT_jump(BetterTimelineButtonsPanel, Panel):
    bl_label = "Time Jump"
    bl_options = {'HIDE_HEADER'}
    bl_ui_units_x = 10

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        scene = context.scene
        layout.prop(scene, "time_jump_unit", expand=True, text="Jump Unit")
        layout.prop(scene, "time_jump_delta", text="Delta")


class BETTER_TIMELINE_PT_playback(BetterTimelineButtonsPanel, Panel):
    bl_label = "Playback"
    bl_options = {'HIDE_HEADER'}
    bl_ui_units_x = 13

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        draw_better_timeline_playback_settings(layout, context)


class BETTER_TIMELINE_PT_active_clip(BetterTimelineSidebarPanel, Panel):
    bl_label = "Clip"

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return space is not None and getattr(space, "active_clip", None) is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        clip = context.space_data.active_clip
        layout.prop(clip, "name")
        row = layout.row()
        row.enabled = False
        row.prop(clip, "type_label", text="Type")

        col = layout.column(align=True)
        col.prop(clip, "start_frame")
        col.prop(clip, "end_frame")
        col.prop(clip, "duration")


class BETTER_TIMELINE_PT_active_track(BetterTimelineSidebarPanel, Panel):
    bl_label = "Track"

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return (
            space is not None and
            getattr(space, "active_clip", None) is None and
            getattr(space, "active_track", None) is not None
        )

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        track = context.space_data.active_track
        layout.prop(track, "name")
        row = layout.row()
        row.enabled = False
        row.prop(track, "type_label", text="Type")


classes = (
    BETTER_TIMELINE_HT_header,
    BETTER_TIMELINE_MT_editor_menus,
    BETTER_TIMELINE_MT_view,
    BETTER_TIMELINE_MT_marker,
    BETTER_TIMELINE_MT_playback,
    BETTER_TIMELINE_PT_playhead_snapping,
    BETTER_TIMELINE_PT_jump,
    BETTER_TIMELINE_PT_playback,
    BETTER_TIMELINE_PT_active_clip,
    BETTER_TIMELINE_PT_active_track,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
