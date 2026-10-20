"""Boîtier Sunton 4827S043 — direction « cyberpunk sombre ».

Trois variantes de BLOCKOUT (volumes, traitements d'arêtes, ventilation, ⚡ en creux) rendues en
studio sombre. Ce n'est pas encore un générateur : aucune cote d'assemblage n'est validée ici, on
choisit d'abord une direction à l'image (règle du propriétaire : propositions + visuels AVANT
implémentation).

    blender -b --factory-startup --python gen_boitier.py -- <variante> <sortie.png> [ech]

variante : 1 = chanfrein brutal · 2 = plaque de blindage · 3 = console à ailettes
"""
import bpy
import bmesh
import math
import os
import sys
from mathutils import Vector

# ── Dimensions réelles (datasheet + mesure du PCB) ───────────────────────────
PCB_W, PCB_H, PCB_T = 123.0, 74.0, 1.6
MOD_W, MOD_H = 105.5, 67.2          # module LCD
ACT_W, ACT_H = 95.04, 53.86         # zone active 480×272
CLR = 1.5                            # jeu PCB / paroi
WALL = 2.4
W = PCB_W + 2 * (CLR + WALL)        # 130,8
H = PCB_H + 2 * (CLR + WALL)        # 81,8
CHIN = 14.0                         # menton sous l'écran : ⚡ + LED d'état
D_BODY = 22.0                        # profondeur coque avant
D_LID = 4.0                          # dos

# ── Variante ────────────────────────────────────────────────────────────────
argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
VAR = int(argv[0]) if argv else 1
OUT = os.path.abspath(argv[1]) if len(argv) > 1 else "/tmp/boitier_v1.png"
SAMP = int(argv[2]) if len(argv) > 2 else 96

ACCENT = (1.0, 0.32, 0.02)          # orange signalétique
BODY_COL = (0.085, 0.090, 0.100)     # anthracite bleuté : le noir pur ne montre aucune forme

# ── Ménage : la scène de démarrage contient un cube, une caméra et 1000 W ──
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
sc = bpy.context.scene


def aim(o, target=(0, 0, 0)):
    """Une lampe AREA émet suivant son -Z local : sans visée, elle éclaire le sol à côté."""
    d = Vector(target) - o.location
    o.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()


def cube(name, size, loc):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc)
    o = bpy.context.object
    o.name = name
    o.scale = (size[0], size[1], size[2])
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return o


def bevel(o, width, segments=1, angle=30):
    m = o.modifiers.new("bev", "BEVEL")
    m.width = width
    m.segments = segments
    m.limit_method = "ANGLE"
    m.angle_limit = math.radians(angle)
    m.harden_normals = False
    bpy.context.view_layer.objects.active = o
    bpy.ops.object.modifier_apply(modifier=m.name)


def boolean(target, cutter, op="DIFFERENCE"):
    m = target.modifiers.new("bool", "BOOLEAN")
    m.operation = op
    m.solver = "EXACT"
    m.object = cutter
    bpy.context.view_layer.objects.active = target
    bpy.ops.object.modifier_apply(modifier=m.name)
    bpy.data.objects.remove(cutter, do_unlink=True)


def bolt(name, pts, thick, loc, mat):
    """Éclair extrudé : polygone dans XY, épaisseur +Z."""
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    verts = [bm.verts.new((x, y, 0.0)) for x, y in pts]
    f = bm.faces.new(verts)
    ret = bmesh.ops.extrude_face_region(bm, geom=[f])
    for e in ret["geom"]:
        if isinstance(e, bmesh.types.BMVert):
            e.co.z += thick
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()
    o = bpy.data.objects.new(name, me)
    bpy.context.collection.objects.link(o)
    o.location = loc
    o.data.materials.append(mat)
    return o


def mat(name, base, rough=0.55, metallic=0.0, emit=None, emit_str=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*base, 1.0)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metallic
    if emit:
        bsdf.inputs["Emission Color"].default_value = (*emit, 1.0)
        bsdf.inputs["Emission Strength"].default_value = emit_str
    return m


M_BODY = mat("corps", BODY_COL, 0.62)
M_EDGE = mat("chant", (0.10, 0.105, 0.115), 0.35, 0.85)
M_ACCENT = mat("accent", (0.9, 0.28, 0.02), 0.40, 0.2, emit=ACCENT, emit_str=3.0)
M_GLASS = mat("dalle", (0.008, 0.010, 0.014), 0.12, 0.0)

# ── Coque avant ────────────────────────────────────────────────────────────
if VAR == 1:
    chamfer, corner, n_slat, plate_off = 3.2, 7.0, 4, 0.0     # arêtes coupées au couteau
elif VAR == 2:
    chamfer, corner, n_slat, plate_off = 1.6, 9.0, 0, 1.2     # plaque rapportée, pas d'ailettes
else:
    chamfer, corner, n_slat, plate_off = 2.2, 5.0, 7, 0.0     # console à lamelles

HF = H + CHIN
body = cube("coque", (W, HF, D_BODY), (0, 0, D_BODY / 2))
bevel(body, chamfer, 1)

# coins coupés à 45° : huit boîtes de coupe, deux par coin
cut = 6.0
for sx in (-1, 1):
    for sy in (-1, 1):
        c = cube("cut", (cut * 2, cut * 2, D_BODY + 6),
                 (sx * (W / 2 - corner / 2 + 0.0), sy * (HF / 2 - corner / 2 + 0.0), D_BODY / 2))
        c.rotation_euler[2] = math.radians(45)
        bpy.context.view_layer.objects.active = c
        bpy.ops.object.transform_apply(rotation=True)
        boolean(body, c)

# ouverture d'écran + lèvre qui couvre le chant du module
opening = cube("ouv", (MOD_W + 0.6, MOD_H + 0.6, D_BODY), (0, CHIN / 2, D_BODY / 2 + 1.2))
boolean(body, opening)

# panneau en creux sur la façade (variante 2 : plaque rapportée)
if plate_off > 0:
    recess = cube("recess", (W - 9, HF - 9, 3.0), (0, 0, D_BODY - 1.0))
    boolean(body, recess)
    plate = cube("plaque", (W - 9.4, HF - 9.4, plate_off + 1.2), (0, 0, D_BODY - 1.0 + plate_off / 2))
    bevel(plate, 0.8, 1)
    plate.data.materials.append(M_EDGE)

# ailettes latérales (fentes traversantes)
if n_slat:
    for side in (-1, 1):
        for k in range(n_slat):
            y = -18 + k * (36 / max(1, n_slat - 1)) if n_slat > 1 else 0
            s = cube("slat", (W * 0.5, 2.2, 7.0), (side * (W / 2 - W * 0.22), y, D_BODY * 0.45))
            boolean(body, s)

# ── ⚡ en creux, rempli d'orange ────────────────────────────────────────────
PTS = [(-0.35, 6.6), (2.9, 6.6), (1.1, 1.7), (3.7, 1.7), (-1.0, -6.6), (0.25, -1.2), (-2.3, -1.2)]
bolt_loc = (-W / 2 + 17.0, -HF / 2 + 7.0, D_BODY - 1.15)
cutter = bolt("bolt_cut", [(x * 1.0, y * 1.0) for x, y in PTS], 4.0, bolt_loc, M_BODY)
boolean(body, cutter)
insert = bolt("bolt_led", [(x * 0.92, y * 0.92) for x, y in PTS], 0.9,
              (bolt_loc[0], bolt_loc[1], D_BODY - 1.15), M_ACCENT)

body.data.materials.append(M_BODY)

# ── Dalle (verre) derrière la lèvre ────────────────────────────────────────
glass = cube("dalle", (MOD_W - 1.6, MOD_H - 1.6, 0.8), (0, CHIN / 2, D_BODY - 3.2))
glass.data.materials.append(M_GLASS)

# ── Dos + grille nid d'abeille ─────────────────────────────────────────────
lid = cube("dos", (W - 2.0, HF - 2.0, D_LID), (0, 0, -D_LID / 2))
bevel(lid, 1.0, 1)
for i in range(-4, 5):
    for j in range(-2, 3):
        x = i * 9.0 + (4.5 if j % 2 else 0.0)
        y = j * 7.8
        c = bpy.data.meshes.new("hex")
        bpy.ops.mesh.primitive_cylinder_add(vertices=6, radius=3.1, depth=D_LID + 3, location=(x, y, -D_LID / 2))
        hexo = bpy.context.object
        bpy.data.meshes.remove(c)
        boolean(lid, hexo)
lid.data.materials.append(M_BODY)

# ── Socle lesté à 15° ──────────────────────────────────────────────────────
base = cube("socle", (104, 84, 8), (0, 0, -D_LID - 4))
bevel(base, 1.4, 1)
tilt = cube("tilt", (W * 0.92, HF * 0.86, 26), (0, 0, -D_LID - 12))
tilt.rotation_euler[0] = math.radians(-15)
bpy.context.view_layer.objects.active = tilt
bpy.ops.object.transform_apply(rotation=True)
base.data.materials.append(M_BODY)

# ── Passage en MÈTRES : la géométrie est cotée en mm, Blender compte en mètres.
# Sans ça la coque fait 130 m de large, la caméra et les lampes sont DEDANS : rendu noir.
for _o in bpy.data.objects:
    if _o.type == "MESH":
        _o.scale = (0.001, 0.001, 0.001)
        _o.location = _o.location * 0.001          # l'origine AUSSI : transform_apply(location)
bpy.ops.object.select_all(action="SELECT")        # ne la reprenait pas ici (mesuré : z restait en mm)
bpy.context.view_layer.objects.active = body
bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
print("[échelle] géométrie en mètres · coque %.3f m de large" % (W / 1000.0))

# ── Studio sombre + rim light orange ───────────────────────────────────────
world = bpy.data.worlds.new("w")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.045, 0.048, 0.055, 1)
world.node_tree.nodes["Background"].inputs[1].default_value = 1.0
sc.world = world

bpy.ops.object.light_add(type="AREA", location=(-0.30, -0.34, 0.30))
key = bpy.context.object
key.data.energy = 5.5
key.data.size = 0.22
key.data.color = (1.0, 0.93, 0.85)
aim(key, (0, 0, 0.008))

bpy.ops.object.light_add(type="AREA", location=(0.34, -0.14, 0.10))
rim = bpy.context.object
rim.data.energy = 3.2
rim.data.size = 0.12
rim.data.color = (1.0, 0.35, 0.08)
aim(rim, (0, 0, 0.008))

bpy.ops.object.light_add(type="AREA", location=(0.07, 0.36, 0.18))
back = bpy.context.object
back.data.energy = 2.2
back.data.size = 0.16
back.data.color = (0.55, 0.7, 1.0)
aim(back, (0, 0, 0.008))

bpy.ops.object.light_add(type="AREA", location=(-0.36, 0.22, 0.14))
fill = bpy.context.object
fill.data.energy = 2.4
fill.data.size = 0.20
fill.data.color = (0.8, 0.86, 1.0)
aim(fill, (0, 0, 0.008))

bpy.ops.object.camera_add(location=(0.19, -0.34, 0.15))
cam = bpy.context.object
cam.data.lens = 66
cam.data.sensor_width = 36
sc.camera = cam
target = Vector((0, 0, 0.004))
d = target - cam.location
cam.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()

# mur de fond sombre, pour que les arêtes se détachent
bg = cube("fond", (1.4, 0.008, 0.9), (0, 0.32, 0.12))
bg.data.materials.append(mat("fondmat", (0.02, 0.021, 0.024), 0.9))

# sol
floor = cube("sol", (1.6, 1.6, 0.004), (0, 0, -(D_LID + 20) / 1000.0))
floor.data.materials.append(mat("solmat", (0.055, 0.057, 0.062), 0.65))

# ── Rendu ──────────────────────────────────────────────────────────────────
sc.render.engine = "CYCLES"
prefs = bpy.context.preferences.addons["cycles"].preferences
for t in ("OPTIX", "CUDA"):
    try:
        prefs.compute_device_type = t
        prefs.refresh_devices()
        prefs.get_devices()
        gpus = [dd for dd in prefs.devices if dd.type != "CPU"]
        if gpus:
            for dd in prefs.devices:
                dd.use = dd.type != "CPU"
            sc.cycles.device = "GPU"
            print(f"[gpu] {t} · {len(gpus)} périphérique(s)")
            break
    except Exception as e:
        print(f"[gpu] {t} indisponible : {e}")
else:
    sc.cycles.device = "CPU"
    print("[gpu] CPU")

sc.cycles.samples = SAMP
sc.cycles.use_denoising = True
try:
    sc.view_settings.view_transform = "Khronos PBR Neutral"
except TypeError:
    sc.view_settings.view_transform = "Standard"
sc.view_settings.look = "None"
sc.view_settings.exposure = -0.25

sc.render.resolution_x = 1400
sc.render.resolution_y = 900
sc.render.resolution_percentage = 100
sc.render.film_transparent = False
# ── Sonde de cadrage : où tombent les pièces dans l'image (0..1) ──
from bpy_extras.object_utils import world_to_camera_view
bpy.context.view_layer.update()
for _ob in (body, glass, lid, base, tilt):
    _bb = [_ob.matrix_world @ Vector(c) for c in _ob.bound_box]
    print("[bbox] %-6s x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f" % (_ob.name,
          min(p.x for p in _bb), max(p.x for p in _bb), min(p.y for p in _bb), max(p.y for p in _bb),
          min(p.z for p in _bb), max(p.z for p in _bb)))
    _us = [world_to_camera_view(sc, cam, _ob.matrix_world @ Vector(c)) for c in _ob.bound_box]
    print("[cadre] %-6s u %.2f..%.2f  v %.2f..%.2f" % (_ob.name,
          min(u.x for u in _us), max(u.x for u in _us), min(u.y for u in _us), max(u.y for u in _us)))

sc.render.filepath = OUT
sc.render.image_settings.file_format = "PNG"

bpy.ops.render.render(write_still=True)
print(f"[rendu] {OUT} (variante {VAR})")
