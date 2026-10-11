"""Re-save a scan asset's textures as JPEG (much smaller than PNG; the visual difference is
negligible at game distances).

blender --background --factory-startup --disable-autoexec --python tools/scan_repack.py -- <in.glb> <out.glb> [quality]
"""
import bpy, sys, os
argv = sys.argv[sys.argv.index("--") + 1:]
src, dst = os.path.abspath(argv[0]), os.path.abspath(argv[1])
quality = int(argv[2]) if len(argv) > 2 else 90
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
for o in bpy.context.scene.objects:
    o.select_set(o.type == 'MESH')
bpy.ops.export_scene.gltf(filepath=dst, use_selection=True, export_format='GLB', export_yup=True, export_apply=True,
                          export_image_format='JPEG', export_jpeg_quality=quality)
print("[repack] %s: %.1f MB -> %.1f MB" % (os.path.basename(dst), os.path.getsize(src) / 1e6, os.path.getsize(dst) / 1e6))
