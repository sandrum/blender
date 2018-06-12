# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

# <pep8 compliant>
import bpy
from bpy.types import Header, Menu, Panel


class TOPBAR_HT_upper_bar(Header):
    bl_space_type = 'TOPBAR'

    def draw(self, context):
        region = context.region

        if region.alignment == 'RIGHT':
            self.draw_right(context)
        else:
            self.draw_left(context)

    def draw_left(self, context):
        layout = self.layout

        window = context.window
        screen = context.screen

        layout.operator("wm.splash", text="", icon='BLENDER', emboss=False)

        INFO_MT_editor_menus.draw_collapsible(context, layout)

        layout.separator()

        if not screen.show_fullscreen:
            layout.template_ID_tabs(
                window, "workspace",
                new="workspace.workspace_add_menu",
                unlink="workspace.workspace_delete",
            )
        else:
            layout.operator(
                "screen.back_to_previous",
                icon='SCREEN_BACK',
                text="Back to Previous",
            )

    def draw_right(self, context):
        layout = self.layout

        window = context.window
        scene = window.scene

        # Active workspace view-layer is retrieved through window, not through workspace.
        layout.template_ID(window, "scene", new="scene.new", unlink="scene.delete")

        row = layout.row(align=True)
        row.template_search(
            window, "view_layer",
            scene, "view_layers",
            new="scene.view_layer_add",
            unlink="scene.view_layer_remove")


class TOPBAR_HT_lower_bar(Header):
    bl_space_type = 'TOPBAR'
    bl_region_type = 'WINDOW'

    def draw(self, context):
        layout = self.layout
        region = context.region

        if region.alignment == 'LEFT':
            self.draw_left(context)
        elif region.alignment == 'RIGHT':
            self.draw_right(context)
        else:
            self.draw_center(context)

    def draw_left(self, context):
        layout = self.layout
        mode = context.mode

        # Active Tool
        # -----------
        from .space_toolsystem_common import ToolSelectPanelHelper
        ToolSelectPanelHelper.draw_active_tool_header(context, layout)

        # Object Mode Options
        # -------------------

        # Example of how toolsettings can be accessed as pop-overs.

        # TODO(campbell): editing options should be after active tool options
        # (obviously separated for from the users POV)
        draw_fn = getattr(_draw_left_context_mode, mode, None)
        if draw_fn is not None:
            draw_fn(context, layout)

        # Note: general mode options should be added to 'draw_right'.
        if mode == 'SCULPT':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".paint_common", category="")
        elif mode == 'PAINT_VERTEX':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".paint_common", category="")
        elif mode == 'PAINT_WEIGHT':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".paint_common", category="")
        elif mode == 'PAINT_TEXTURE':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".paint_common", category="")
        elif mode == 'EDIT_ARMATURE':
            pass
        elif mode == 'EDIT_CURVE':
            pass
        elif mode == 'EDIT_MESH':
            pass
        elif mode == 'POSE':
            pass
        elif mode == 'PARTICLE':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".paint_common", category="")

    def draw_center(self, context):
        pass

    def draw_right(self, context):
        layout = self.layout

        # General options, note, these _could_ display at the RHS of the draw_left callback.
        # we just want them not to be confused with tool options.
        mode = context.mode

        if mode == 'SCULPT':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".sculpt_mode", category="")
        elif mode == 'PAINT_VERTEX':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".vertexpaint", category="")
        elif mode == 'PAINT_WEIGHT':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".weightpaint", category="")
        elif mode == 'PAINT_TEXTURE':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".imagepaint", category="")
        elif mode == 'EDIT_ARMATURE':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".armature_edit", category="")
        elif mode == 'EDIT_CURVE':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".curve_edit", category="")
        elif mode == 'EDIT_MESH':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".mesh_edit", category="")
        elif mode == 'POSE':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".posemode", category="")
        elif mode == 'PARTICLE':
            layout.popover_group(space_type='PROPERTIES', region_type='WINDOW', context=".particlemode", category="")


class _draw_left_context_mode:
    @staticmethod
    def SCULPT(context, layout):
        brush = context.tool_settings.sculpt.brush
        if brush is None:
            return

        from .properties_paint_common import UnifiedPaintPanel

        UnifiedPaintPanel.prop_unified_size(layout, context, brush, "size", slider=True, text="Radius")
        UnifiedPaintPanel.prop_unified_strength(layout, context, brush, "strength", slider=True, text="Strength")
        layout.prop(brush, "direction", text="", expand=True)

    def PAINT_TEXTURE(context, layout):
        brush = context.tool_settings.vertex_paint.brush
        if brush is None:
            return

        from .properties_paint_common import UnifiedPaintPanel

        layout.prop(brush, "color", text="")
        UnifiedPaintPanel.prop_unified_size(layout, context, brush, "size", slider=True, text="Radius")
        UnifiedPaintPanel.prop_unified_strength(layout, context, brush, "strength", slider=True, text="Strength")

    def PAINT_VERTEX(context, layout):
        brush = context.tool_settings.vertex_paint.brush
        if brush is None:
            return

        from .properties_paint_common import UnifiedPaintPanel

        layout.prop(brush, "color", text="")
        UnifiedPaintPanel.prop_unified_size(layout, context, brush, "size", slider=True, text="Radius")
        UnifiedPaintPanel.prop_unified_strength(layout, context, brush, "strength", slider=True, text="Strength")

    def PAINT_WEIGHT(context, layout):
        brush = context.tool_settings.weight_paint.brush
        if brush is None:
            return

        from .properties_paint_common import UnifiedPaintPanel

        UnifiedPaintPanel.prop_unified_weight(layout, context, brush, "weight", slider=True, text="Weight")
        UnifiedPaintPanel.prop_unified_size(layout, context, brush, "size", slider=True, text="Radius")
        UnifiedPaintPanel.prop_unified_strength(layout, context, brush, "strength", slider=True, text="Strength")


class TOPBAR_PT_pivot_point(Panel):
    bl_space_type = 'TOPBAR'
    bl_region_type = 'HEADER'
    bl_label = "Pivot Point"

    def draw(self, context):
        toolsettings = context.tool_settings
        obj = context.active_object
        mode = context.mode

        layout = self.layout
        col = layout.column()
        col.label("Pivot Point")
        col.prop(toolsettings, "transform_pivot_point", expand=True)

        col.separator()

        if (obj is None) or (mode in {'OBJECT', 'POSE', 'WEIGHT_PAINT'}):
            col.prop(
                toolsettings,
                "use_transform_pivot_point_align",
                text="Center Points Only",
            )


class TOPBAR_PT_snapping(Panel):
    bl_space_type = 'TOPBAR'
    bl_region_type = 'HEADER'
    bl_label = "Snapping"

    def draw(self, context):
        toolsettings = context.tool_settings
        snap_elements = toolsettings.snap_elements
        obj = context.active_object
        mode = context.mode
        object_mode = 'OBJECT' if obj is None else obj.mode

        layout = self.layout
        col = layout.column()
        col.label("Snapping")
        col.prop(toolsettings, "snap_elements", expand=True)

        col.separator()
        if 'INCREMENT' in snap_elements:
            col.prop(toolsettings, "use_snap_grid_absolute")

        if snap_elements != {'INCREMENT'}:
            col.label("Target")
            row = col.row(align=True)
            row.prop(toolsettings, "snap_target", expand=True)

            if obj:
                if object_mode == 'EDIT':
                    col.prop(toolsettings, "use_snap_self")
                if object_mode in {'OBJECT', 'POSE', 'EDIT'}:
                    col.prop(toolsettings, "use_snap_align_rotation", text="Align Rotation")

            if 'FACE' in snap_elements:
                col.prop(toolsettings, "use_snap_project", text="Project Elements")

            if 'VOLUME' in snap_elements:
                col.prop(toolsettings, "use_snap_peel_object")

        # Auto-Merge Editing
        if obj:
            if (object_mode == 'EDIT' and obj.type == 'MESH'):
                col.prop(toolsettings, "use_mesh_automerge", icon='AUTOMERGE_ON')


class INFO_MT_editor_menus(Menu):
    bl_idname = "INFO_MT_editor_menus"
    bl_label = ""

    def draw(self, context):
        self.draw_menus(self.layout, context)

    @staticmethod
    def draw_menus(layout, context):
        layout.menu("INFO_MT_file")
        layout.menu("INFO_MT_edit")

        layout.menu("INFO_MT_render")

        layout.menu("INFO_MT_window")
        layout.menu("INFO_MT_help")


class INFO_MT_file(Menu):
    bl_label = "File"

    def draw(self, context):
        layout = self.layout

        layout.operator_context = 'INVOKE_AREA'
        layout.operator("wm.read_homefile", text="New", icon='NEW')
        layout.operator("wm.open_mainfile", text="Open...", icon='FILE_FOLDER')
        layout.menu("INFO_MT_file_open_recent", icon='OPEN_RECENT')
        layout.operator("wm.revert_mainfile", icon='FILE_REFRESH')
        layout.operator("wm.recover_last_session", icon='RECOVER_LAST')
        layout.operator("wm.recover_auto_save", text="Recover Auto Save...", icon='RECOVER_AUTO')

        layout.separator()

        layout.operator_context = 'EXEC_AREA' if context.blend_data.is_saved else 'INVOKE_AREA'
        layout.operator("wm.save_mainfile", text="Save", icon='FILE_TICK')

        layout.operator_context = 'INVOKE_AREA'
        layout.operator("wm.save_as_mainfile", text="Save As...", icon='SAVE_AS')
        layout.operator_context = 'INVOKE_AREA'
        layout.operator("wm.save_as_mainfile", text="Save Copy...", icon='SAVE_COPY').copy = True

        layout.separator()

        layout.operator_context = 'INVOKE_AREA'
        layout.operator("wm.save_homefile", icon='SAVE_PREFS')
        layout.operator("wm.read_factory_settings", icon='LOAD_FACTORY')

        layout.separator()

        layout.operator_context = 'INVOKE_AREA'
        layout.operator("wm.link", text="Link", icon='LINK_BLEND')
        layout.operator("wm.append", text="Append", icon='APPEND_BLEND')
        layout.menu("INFO_MT_file_previews")

        layout.separator()

        layout.menu("INFO_MT_file_import", icon='IMPORT')
        layout.menu("INFO_MT_file_export", icon='EXPORT')

        layout.separator()

        layout.menu("INFO_MT_file_external_data", icon='EXTERNAL_DATA')

        layout.separator()

        layout.operator_context = 'EXEC_AREA'
        if bpy.data.is_dirty and context.user_preferences.view.use_quit_dialog:
            layout.operator_context = 'INVOKE_SCREEN'  # quit dialog
        layout.operator("wm.quit_blender", text="Quit", icon='QUIT')


class INFO_MT_file_import(Menu):
    bl_idname = "INFO_MT_file_import"
    bl_label = "Import"

    def draw(self, context):
        if bpy.app.build_options.collada:
            self.layout.operator("wm.collada_import", text="Collada (Default) (.dae)")
        if bpy.app.build_options.alembic:
            self.layout.operator("wm.alembic_import", text="Alembic (.abc)")


class INFO_MT_file_export(Menu):
    bl_idname = "INFO_MT_file_export"
    bl_label = "Export"

    def draw(self, context):
        if bpy.app.build_options.collada:
            self.layout.operator("wm.collada_export", text="Collada (Default) (.dae)")
        if bpy.app.build_options.alembic:
            self.layout.operator("wm.alembic_export", text="Alembic (.abc)")


class INFO_MT_file_external_data(Menu):
    bl_label = "External Data"

    def draw(self, context):
        layout = self.layout

        icon = 'CHECKBOX_HLT' if bpy.data.use_autopack else 'CHECKBOX_DEHLT'
        layout.operator("file.autopack_toggle", icon=icon)

        layout.separator()

        pack_all = layout.row()
        pack_all.operator("file.pack_all")
        pack_all.active = not bpy.data.use_autopack

        unpack_all = layout.row()
        unpack_all.operator("file.unpack_all")
        unpack_all.active = not bpy.data.use_autopack

        layout.separator()

        layout.operator("file.make_paths_relative")
        layout.operator("file.make_paths_absolute")
        layout.operator("file.report_missing_files")
        layout.operator("file.find_missing_files")


class INFO_MT_file_previews(Menu):
    bl_label = "Data Previews"

    def draw(self, context):
        layout = self.layout

        layout.operator("wm.previews_ensure")
        layout.operator("wm.previews_batch_generate")

        layout.separator()

        layout.operator("wm.previews_clear")
        layout.operator("wm.previews_batch_clear")


class INFO_MT_game(Menu):
    bl_label = "Game"

    def draw(self, context):
        layout = self.layout

        gs = context.scene.game_settings

        layout.operator("view3d.game_start")

        layout.separator()

        layout.prop(gs, "show_debug_properties")
        layout.prop(gs, "show_framerate_profile")
        layout.prop(gs, "show_physics_visualization")
        layout.prop(gs, "use_deprecation_warnings")
        layout.prop(gs, "use_animation_record")
        layout.separator()
        layout.prop(gs, "use_auto_start")


class INFO_MT_render(Menu):
    bl_label = "Render"

    def draw(self, context):
        layout = self.layout

        rd = context.scene.render

        layout.operator("render.render", text="Render Image", icon='RENDER_STILL').use_viewport = True
        props = layout.operator("render.render", text="Render Animation", icon='RENDER_ANIMATION')
        props.animation = True
        props.use_viewport = True
        layout.operator("sound.mixdown", text="Render Audio", icon='PLAY_AUDIO')

        layout.separator()

        layout.prop_menu_enum(rd, "display_mode", text="Display Mode", icon='IMAGE_COL')
        layout.prop(rd, "use_lock_interface", text="Lock Interface")

        layout.separator()

        props = layout.operator("render.opengl", text="OpenGL Render Image", icon='RENDER_STILL')
        props.view_context = False
        props = layout.operator("render.opengl", text="OpenGL Render Animation", icon='RENDER_ANIMATION')
        props.view_context = False
        props.animation = True
        layout.menu("INFO_MT_opengl_render", icon='SETTINGS')

        layout.separator()

        layout.operator("render.view_show")
        layout.operator("render.play_rendered_anim", icon='PLAY')


class INFO_MT_opengl_render(Menu):
    bl_label = "OpenGL Render Options"

    def draw(self, context):
        layout = self.layout

        rd = context.scene.render
        layout.prop(rd, "use_antialiasing")
        layout.prop(rd, "use_full_sample")

        layout.prop_menu_enum(rd, "antialiasing_samples")
        layout.prop_menu_enum(rd, "alpha_mode")


class INFO_MT_edit(Menu):
    bl_label = "Edit"

    def draw(self, context):
        layout = self.layout

        layout.operator("ed.undo")
        layout.operator("ed.redo")

        layout.separator()

        layout.operator("ed.undo_history", text="Undo History...")

        layout.separator()

        layout.operator("screen.repeat_last")
        layout.operator("screen.repeat_history", text="Repeat History...")

        layout.separator()

        layout.operator("screen.redo_last", text="Adjust Last Operation...")

        layout.separator()

        layout.operator("screen.userpref_show", text="User Preferences...", icon='PREFERENCES')


class INFO_MT_window(Menu):
    bl_label = "Window"

    def draw(self, context):
        import sys

        layout = self.layout

        layout.operator("wm.window_new")
        layout.operator("wm.window_fullscreen_toggle", icon='FULLSCREEN_ENTER')

        layout.separator()

        layout.operator("screen.screenshot")
        layout.operator("screen.screencast")

        if sys.platform[:3] == "win":
            layout.separator()
            layout.operator("wm.console_toggle", icon='CONSOLE')

        if context.scene.render.use_multiview:
            layout.separator()
            layout.operator("wm.set_stereo_3d", icon='CAMERA_STEREO')


class INFO_MT_help(Menu):
    bl_label = "Help"

    def draw(self, context):
        layout = self.layout

        layout.operator(
            "wm.url_open", text="Manual", icon='HELP',
        ).url = "https://docs.blender.org/manual/en/dev/"
        layout.operator(
            "wm.url_open", text="Release Log", icon='URL',
        ).url = "http://wiki.blender.org/index.php/Dev:Ref/Release_Notes/%d.%d" % bpy.app.version[:2]
        layout.separator()

        layout.operator(
            "wm.url_open", text="Blender Website", icon='URL',
        ).url = "https://www.blender.org"
        layout.operator(
            "wm.url_open", text="Blender Store", icon='URL',
        ).url = "https://store.blender.org"
        layout.operator(
            "wm.url_open", text="Developer Community", icon='URL',
        ).url = "https://www.blender.org/get-involved/"
        layout.operator(
            "wm.url_open", text="User Community", icon='URL',
        ).url = "https://www.blender.org/support/user-community"
        layout.separator()
        layout.operator(
            "wm.url_open", text="Report a Bug", icon='URL',
        ).url = "https://developer.blender.org/maniphest/task/edit/form/1"
        layout.separator()

        layout.operator(
            "wm.url_open", text="Python API Reference", icon='URL',
        ).url = bpy.types.WM_OT_doc_view._prefix

        layout.operator("wm.operator_cheat_sheet", icon='TEXT')
        layout.operator("wm.sysinfo", icon='TEXT')
        layout.separator()

        layout.operator("wm.splash", icon='BLENDER')


classes = (
    TOPBAR_HT_upper_bar,
    TOPBAR_HT_lower_bar,
    TOPBAR_PT_pivot_point,
    TOPBAR_PT_snapping,
    INFO_MT_editor_menus,
    INFO_MT_file,
    INFO_MT_file_import,
    INFO_MT_file_export,
    INFO_MT_file_external_data,
    INFO_MT_file_previews,
    INFO_MT_edit,
    INFO_MT_game,
    INFO_MT_render,
    INFO_MT_opengl_render,
    INFO_MT_window,
    INFO_MT_help,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
