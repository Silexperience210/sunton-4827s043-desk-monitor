import bpy
import sys
import os

OUT = "/home/silex/sunton-4827s043-desk-monitor/docs/preview.png"
STL_DIR = "/home/silex/sunton-4827s043-desk-monitor/stl"

# ---- Cleanup ----
for obj in list(bpy.data.objects):
    bpy.data.objects.remove(obj, do_unlink=True)

# ---- Import STLs ----
colors = {
    "front-case.stl": (0.2, 0.7, 0.9),
    "back-cover.stl": (0.9, 0.3, 0.6),
    "desk-stand.stl": (1.0, 0.8, 0.2),
}

objects = []
for name, color in colors.items():
    path = os.path.join(STL_DIR, name)
    bpy.ops.wm.stl_import(filepath=path, global_scale=1.0)
    obj = bpy.context.selected_objects[0]
    obj.name = name.replace(".stl", "")
    # center origin
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.origin_set(type="GEOMETRY_ORIGIN", center="BOUNDS")
    # material
    mat = bpy.data.materials.new(name=obj.name + "-mat")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.4
    obj.data.materials.append(mat)
    objects.append(obj)

# ---- Layout : exploded on ground plane ----
front, back, stand = objects
front.location = (0, 0, 0)
back.location = (150, -20, 0)
stand.location = (-130, -60, 0)
stand.rotation_euler = (0, 0, 20)

# ---- Ground plane ----
plane_mesh = bpy.data.meshes.new("Ground")
plane = bpy.data.objects.new("Ground", plane_mesh)
bpy.context.scene.collection.objects.link(plane)
# Create plane geometry
import bmesh
bm = bmesh.new()
bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=200)
bm.to_mesh(plane_mesh)
bm.free()
plane.location = (0, 0, -40)
plane_mat = bpy.data.materials.new(name="ground")
plane_mat.use_nodes = True
plane_mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.08, 0.08, 0.1, 1.0)
plane.data.materials.append(plane_mat)

# ---- Camera ----
cam_data = bpy.data.cameras.new("Camera")
cam = bpy.data.objects.new("Camera", cam_data)
bpy.context.scene.collection.objects.link(cam)
bpy.context.scene.camera = cam
# Frame all objects
cam.location = (380, -480, 260)
cam.rotation_euler = (1.08, 0, 0.72)
cam.data.lens = 35

# ---- Lights ----
light_data = bpy.data.lights.new("Key", type="SUN")
light = bpy.data.objects.new("Key", light_data)
bpy.context.scene.collection.objects.link(light)
light.location = (100, -100, 150)
light.rotation_euler = (0.8, 0, 0.5)
light.data.energy = 4.0

fill_data = bpy.data.lights.new("Fill", type="AREA")
fill = bpy.data.objects.new("Fill", fill_data)
bpy.context.scene.collection.objects.link(fill)
fill.location = (-120, -50, 100)
fill.rotation_euler = (1.0, 0, -0.8)
fill.data.energy = 150
fill.data.size = 80

rim_data = bpy.data.lights.new("Rim", type="AREA")
rim = bpy.data.objects.new("Rim", rim_data)
bpy.context.scene.collection.objects.link(rim)
rim.location = (0, 120, 80)
rim.rotation_euler = (1.4, 0, 3.14)
rim.data.energy = 100
rim.data.size = 60

# ---- Render settings ----
scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = 48
scene.render.resolution_x = 1280
scene.render.resolution_y = 720
scene.render.filepath = OUT
scene.render.image_settings.file_format = "PNG"
scene.view_settings.view_transform = "Khronos PBR Neutral"

# ---- Render ----
bpy.ops.render.render(write_still=True)
print("Saved preview to", OUT)
