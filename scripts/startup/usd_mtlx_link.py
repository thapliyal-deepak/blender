# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Link an external MaterialX (.mtlx) document to a Blender material, and browse/fetch
materials on demand from the AMD GPUOpen MaterialX Library (https://matlib.gpuopen.com).

This is the "reference an external .mtlx into USD" workflow (as opposed to converting
Blender's shader nodes to MaterialX on export):

- Imports the .mtlx for viewport display (an approximated Principled node tree, via the
  built-in USD MaterialX importer) and assigns it to the active object.
- Tags the material with a ``usd_mtlx_reference`` custom property.  On USD export, the
  C++ writer (``usd_writer_material.cc``) authors the Material prim as a USD reference to
  that .mtlx instead of converting nodes.

The GPUOpen browser fetches on demand: it searches the online library (with thumbnails
and pagination), downloads the selected material's package (at a chosen resolution) into a
local cache, then links the extracted .mtlx exactly like a local file.  All of this lives
in Properties > Material, deliberately OUTSIDE the USD Stage Editor.
"""

import json
import math
import os
import shutil
import urllib.parse
import urllib.request
import zipfile

import bpy
import bpy.utils.previews
from bpy.types import Operator, Panel
from bpy.props import StringProperty
from bpy_extras.io_utils import ImportHelper

MTLX_REF_PROP = "usd_mtlx_reference"

GPUOPEN_API = "https://api.matlib.gpuopen.com/api"
GPUOPEN_IMG = "https://image.matlib.gpuopen.com"
_USER_AGENT = "Blender-USD-MaterialX"
PAGE_SIZE = 12

# State from the most recent fetch.
#   _gpuopen_results: list of {"id", "title", "packages": [ids], "render", "icon_id"}
#   _gpuopen_total:   total match count reported by the API (for pagination)
#   _previews:        bpy preview collection holding the thumbnail icons
_gpuopen_results = []
_gpuopen_total = 0
_previews = None


# -------------------------------------------------------------------------------------
# Shared link logic
# -------------------------------------------------------------------------------------

def _link_external_mtlx(context, path, report):
    """Import a .mtlx for the viewport, mark it for USD reference export, and assign it to
    the active object.  Returns the operator result set."""
    if not path or not path.lower().endswith(".mtlx") or not os.path.exists(path):
        report({'ERROR'}, "MaterialX file not found: %s" % path)
        return {'CANCELLED'}

    obj = context.object

    mats_before = set(bpy.data.materials.keys())
    objs_before = set(bpy.data.objects.keys())
    try:
        bpy.ops.wm.usd_import(filepath=path,
                              import_all_materials=True,
                              import_usd_preview=True)
    except RuntimeError as ex:
        report({'ERROR'}, "USD import failed: %s" % ex)
        return {'CANCELLED'}

    new_mats = [bpy.data.materials[n] for n in bpy.data.materials.keys()
                if n not in mats_before]
    for name in list(bpy.data.objects.keys()):
        if name not in objs_before:
            bpy.data.objects.remove(bpy.data.objects[name], do_unlink=True)

    mat = new_mats[0] if new_mats else None
    if mat is None:
        # No UsdShadeMaterial in the file: create an empty material so the USD reference
        # still exports and binds.
        mat = bpy.data.materials.new(name=bpy.path.display_name_from_filepath(path))
        mat.use_nodes = True

    mat[MTLX_REF_PROP] = path

    if obj is not None and getattr(obj.data, "materials", None) is not None:
        if obj.material_slots:
            obj.material_slots[obj.active_material_index].material = mat
        else:
            obj.data.materials.append(mat)

    report({'INFO'}, "Linked MaterialX: %s" % bpy.path.basename(path))
    return {'FINISHED'}


def _apply_mtlx(context, path, report):
    """Apply a .mtlx.  If a USD Stage Editor has a stage open with a prim selected, bind the
    material into that stage (which records the change on the layer so it shows a Save button and
    reloads in Unreal); otherwise fall back to linking a Blender material for USD export."""
    if not path or not path.lower().endswith(".mtlx") or not os.path.exists(path):
        report({'ERROR'}, "MaterialX file not found: %s" % path)
        return {'CANCELLED'}

    # Prefer binding into the open stage (dirties the layer, like any other stage edit).
    # This only succeeds when a USD Stage Editor has a stage open AND a prim is selected.
    can_bind = "bind_mtlx_material" in dir(bpy.ops.usd_stage)
    if can_bind:
        try:
            if bpy.ops.usd_stage.bind_mtlx_material.poll():
                res = bpy.ops.usd_stage.bind_mtlx_material('EXEC_DEFAULT', filepath=path)
                if 'FINISHED' in res:
                    report({'INFO'},
                           "Bound into the open USD stage — the layer now shows unsaved changes; "
                           "Save the layer, then reload in Unreal")
                    return {'FINISHED'}
        except RuntimeError:
            pass

    # Not bound: either no USD Stage Editor is open, or no prim is selected in it.
    result = _link_external_mtlx(context, path, report)
    if 'FINISHED' in result:
        report({'WARNING'},
               "Linked as a Blender material only (NOT added to the open stage). To put it in the "
               "stage, select the target prim in the USD Stage Editor first, then apply again.")
    return result


# -------------------------------------------------------------------------------------
# GPUOpen network helpers
# -------------------------------------------------------------------------------------

def _http_get_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": _USER_AGENT})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode("utf-8"))


def _http_download(url, dest):
    req = urllib.request.Request(url, headers={"User-Agent": _USER_AGENT})
    with urllib.request.urlopen(req, timeout=300) as r, open(dest, "wb") as f:
        shutil.copyfileobj(r, f)


def _cache_dir():
    return bpy.utils.user_resource('DATAFILES', path="gpuopen_mtlx", create=True)


def _thumb_dir():
    return bpy.utils.user_resource('DATAFILES', path="gpuopen_mtlx/thumbs", create=True)


def _find_mtlx(folder):
    for root, _dirs, files in os.walk(folder):
        for f in files:
            if f.lower().endswith(".mtlx"):
                return os.path.join(root, f)
    return None


def _ensure_thumbnail(render_id):
    """Download (once) a render's thumbnail JPEG and return its local path, or None."""
    if not render_id:
        return None
    dest = os.path.join(_thumb_dir(), "%s.jpeg" % render_id)
    if not os.path.exists(dest):
        try:
            _http_download("%s/%s_thumbnail.jpeg" % (GPUOPEN_IMG, render_id), dest)
        except Exception:
            return None
    return dest if os.path.exists(dest) else None


def _fetch_page(context, report):
    """Fetch the current search page into module state and (re)load thumbnails."""
    global _gpuopen_results, _gpuopen_total

    wm = context.window_manager
    query = wm.usd_mtlx_search.strip()
    page = max(0, wm.usd_mtlx_page)
    offset = page * PAGE_SIZE

    url = "%s/materials/?limit=%d&offset=%d&status=Published" % (GPUOPEN_API, PAGE_SIZE, offset)
    if query:
        url += "&search=" + urllib.parse.quote(query)
    try:
        data = _http_get_json(url)
    except Exception as ex:
        report({'ERROR'}, "GPUOpen fetch failed: %s" % ex)
        return False

    _gpuopen_total = int(data.get("count", 0))
    results = []
    for m in data.get("results", []):
        if not m.get("id"):
            continue
        renders = m.get("renders") or []
        results.append({
            "id": m["id"],
            "title": m.get("title") or m["id"],
            "packages": m.get("packages") or [],
            "render": renders[0] if renders else None,
            "icon_id": 0,
        })
    _gpuopen_results = results

    # Load thumbnails as preview icons for this page.
    if _previews is not None:
        _previews.clear()
        for entry in _gpuopen_results:
            thumb = _ensure_thumbnail(entry["render"])
            if thumb:
                try:
                    entry["icon_id"] = _previews.load(entry["id"], thumb, 'IMAGE').icon_id
                except Exception:
                    entry["icon_id"] = 0

    if context.screen:
        for area in context.screen.areas:
            area.tag_redraw()
    return True


# -------------------------------------------------------------------------------------
# Operators
# -------------------------------------------------------------------------------------

class MATERIAL_OT_usd_link_external_mtlx(Operator, ImportHelper):
    """Import an external MaterialX file for the viewport and reference it on USD export"""

    bl_idname = "material.usd_link_external_mtlx"
    bl_label = "Link External MaterialX"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".mtlx"
    filter_glob: StringProperty(default="*.mtlx", options={'HIDDEN'})

    def execute(self, context):
        return _apply_mtlx(context, self.filepath, self.report)


class MATERIAL_OT_usd_clear_external_mtlx(Operator):
    """Remove the external MaterialX reference from the active material"""

    bl_idname = "material.usd_clear_external_mtlx"
    bl_label = "Clear External MaterialX Reference"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        mat = getattr(context, "material", None)
        return mat is not None and MTLX_REF_PROP in mat

    def execute(self, context):
        mat = context.material
        if MTLX_REF_PROP in mat:
            del mat[MTLX_REF_PROP]
        return {'FINISHED'}


class MATERIAL_OT_usd_gpuopen_search(Operator):
    """Search the AMD GPUOpen MaterialX library"""

    bl_idname = "material.usd_gpuopen_search"
    bl_label = "Search GPUOpen Library"
    bl_options = {'REGISTER'}

    def execute(self, context):
        context.window_manager.usd_mtlx_page = 0
        if not _fetch_page(context, self.report):
            return {'CANCELLED'}
        self.report({'INFO'}, "Showing %d of %d materials"
                    % (len(_gpuopen_results), _gpuopen_total))
        return {'FINISHED'}


class MATERIAL_OT_usd_gpuopen_page(Operator):
    """Go to the previous/next page of GPUOpen results"""

    bl_idname = "material.usd_gpuopen_page"
    bl_label = "GPUOpen Page"
    bl_options = {'REGISTER'}

    delta: bpy.props.IntProperty(default=1, options={'HIDDEN'})

    def execute(self, context):
        wm = context.window_manager
        num_pages = max(1, math.ceil(_gpuopen_total / PAGE_SIZE))
        wm.usd_mtlx_page = min(max(0, wm.usd_mtlx_page + self.delta), num_pages - 1)
        if not _fetch_page(context, self.report):
            return {'CANCELLED'}
        return {'FINISHED'}


class MATERIAL_OT_usd_gpuopen_apply(Operator):
    """Download this material at the chosen resolution and link its MaterialX"""

    bl_idname = "material.usd_gpuopen_apply"
    bl_label = "Download & Link"
    bl_options = {'REGISTER', 'UNDO'}

    material_id: StringProperty(options={'HIDDEN'})

    def execute(self, context):
        wanted = context.window_manager.usd_mtlx_resolution.lower()

        # Resolve the material's package list (prefer the cached search entry).
        entry = next((m for m in _gpuopen_results if m["id"] == self.material_id), None)
        packages = entry["packages"] if entry else None
        if not packages:
            try:
                detail = _http_get_json("%s/materials/%s/" % (GPUOPEN_API, self.material_id))
                packages = detail.get("packages") or []
            except Exception as ex:
                self.report({'ERROR'}, "Could not fetch material detail: %s" % ex)
                return {'CANCELLED'}

        # Find the package whose label matches the requested resolution/bit-depth.
        pkg_id = None
        try:
            for pid in packages:
                pkg = _http_get_json("%s/packages/%s/" % (GPUOPEN_API, pid))
                if pkg.get("label", "").strip().lower() == wanted:
                    pkg_id = pid
                    break
        except Exception as ex:
            self.report({'ERROR'}, "Could not fetch package info: %s" % ex)
            return {'CANCELLED'}

        if not pkg_id:
            self.report({'ERROR'}, "No '%s' package for this material" % wanted)
            return {'CANCELLED'}

        # Download + extract into a per-(material, resolution) cache folder.
        dest = os.path.join(_cache_dir(),
                            "%s_%s" % (self.material_id, wanted.replace(" ", "_")))
        os.makedirs(dest, exist_ok=True)
        mtlx_path = _find_mtlx(dest)
        if mtlx_path is None:
            zip_path = os.path.join(dest, "package.zip")
            try:
                _http_download("%s/packages/%s/download/" % (GPUOPEN_API, pkg_id), zip_path)
                with zipfile.ZipFile(zip_path) as z:
                    z.extractall(dest)
            except Exception as ex:
                self.report({'ERROR'}, "Download failed: %s" % ex)
                return {'CANCELLED'}
            finally:
                if os.path.exists(zip_path):
                    os.remove(zip_path)
            mtlx_path = _find_mtlx(dest)

        if mtlx_path is None:
            self.report({'ERROR'}, "No .mtlx found in downloaded package")
            return {'CANCELLED'}

        return _apply_mtlx(context, mtlx_path, self.report)


# -------------------------------------------------------------------------------------
# Panels
# -------------------------------------------------------------------------------------

class MATERIAL_PT_usd_external_mtlx(Panel):
    bl_label = "USD External MaterialX"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "material"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return context.material is not None

    def draw(self, context):
        layout = self.layout
        mat = context.material
        layout.operator(MATERIAL_OT_usd_link_external_mtlx.bl_idname,
                        text="Link External MaterialX...", icon='NODE_MATERIAL')

        ref = mat.get(MTLX_REF_PROP) if mat else None
        col = layout.column(align=True)
        if ref:
            col.label(text="Referenced on USD export:", icon='CHECKMARK')
            col.label(text=ref)
            col.operator(MATERIAL_OT_usd_clear_external_mtlx.bl_idname,
                         text="Clear Reference", icon='X')
        else:
            col.label(text="No external .mtlx linked.", icon='INFO')

        # Author standalone .mtlx files from the active object's materials.
        layout.separator()
        col = layout.column(align=True)
        col.label(text="Author MaterialX from Blender nodes:")
        # WM_OT_materialx_export is only registered when built WITH_USD + WITH_MATERIALX.
        # bpy.ops introspection via dir() reliably reflects C++ operator registration.
        if "materialx_export" in dir(bpy.ops.wm):
            col.operator("wm.materialx_export",
                         text="Export as MaterialX (.mtlx)...", icon='EXPORT')
        else:
            col.label(text="(build has no MaterialX support)", icon='INFO')


class MATERIAL_PT_usd_gpuopen(Panel):
    bl_label = "AMD GPUOpen Library"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "material"
    bl_parent_id = "MATERIAL_PT_usd_external_mtlx"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        wm = context.window_manager

        row = layout.row(align=True)
        row.prop(wm, "usd_mtlx_search", text="", icon='VIEWZOOM')
        row.operator(MATERIAL_OT_usd_gpuopen_search.bl_idname, text="Search")
        layout.prop(wm, "usd_mtlx_resolution", text="Resolution")

        if not _gpuopen_results:
            layout.label(text="Search the online MaterialX library.", icon='INFO')
            return

        # Thumbnail grid: one clickable cell per material.
        grid = layout.grid_flow(row_major=True, columns=3, even_columns=True,
                                even_rows=True, align=True)
        for entry in _gpuopen_results:
            cell = grid.column(align=True)
            if entry.get("icon_id"):
                cell.template_icon(icon_value=entry["icon_id"], scale=5.0)
            op = cell.operator(MATERIAL_OT_usd_gpuopen_apply.bl_idname,
                               text=entry["title"], icon='IMPORT')
            op.material_id = entry["id"]

        # Pagination footer.
        num_pages = max(1, math.ceil(_gpuopen_total / PAGE_SIZE))
        page = wm.usd_mtlx_page
        nav = layout.row(align=True)
        sub = nav.row(align=True)
        sub.enabled = page > 0
        sub.operator(MATERIAL_OT_usd_gpuopen_page.bl_idname, text="", icon='TRIA_LEFT').delta = -1
        nav.label(text="Page %d / %d  (%d materials)" % (page + 1, num_pages, _gpuopen_total))
        sub = nav.row(align=True)
        sub.enabled = page < num_pages - 1
        sub.operator(MATERIAL_OT_usd_gpuopen_page.bl_idname, text="", icon='TRIA_RIGHT').delta = 1


classes = (
    MATERIAL_OT_usd_link_external_mtlx,
    MATERIAL_OT_usd_clear_external_mtlx,
    MATERIAL_OT_usd_gpuopen_search,
    MATERIAL_OT_usd_gpuopen_page,
    MATERIAL_OT_usd_gpuopen_apply,
    MATERIAL_PT_usd_external_mtlx,
    MATERIAL_PT_usd_gpuopen,
)


def register():
    global _previews
    bpy.types.WindowManager.usd_mtlx_search = StringProperty(
        name="Search",
        description="Filter the GPUOpen material library (leave empty to list all)",
        default="",
    )
    bpy.types.WindowManager.usd_mtlx_page = bpy.props.IntProperty(
        name="Page", default=0, min=0, options={'HIDDEN'},
    )
    bpy.types.WindowManager.usd_mtlx_resolution = bpy.props.EnumProperty(
        name="Resolution",
        description="Texture resolution / bit depth to download",
        items=[
            ("1K 8B", "1K (8-bit)", "1024 px, 8-bit  (~6 MB/material)"),
            ("2K 8B", "2K (8-bit)", "2048 px, 8-bit  (~25 MB/material)"),
            ("4K 8B", "4K (8-bit)", "4096 px, 8-bit  (~100 MB/material)"),
            ("4K 16B", "4K (16-bit)", "4096 px, 16-bit (~230 MB/material)"),
        ],
        default="2K 8B",
    )
    try:
        _previews = bpy.utils.previews.new()
    except Exception:
        _previews = None  # Thumbnails unavailable (e.g. background mode); panel still works.
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    global _previews
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
    if _previews is not None:
        bpy.utils.previews.remove(_previews)
        _previews = None
    del bpy.types.WindowManager.usd_mtlx_search
    del bpy.types.WindowManager.usd_mtlx_page
    del bpy.types.WindowManager.usd_mtlx_resolution


if __name__ == "__main__":
    register()
