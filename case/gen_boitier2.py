"""Boîtier Sunton 4827S043 — passe 2, direction cyberpunk sombre SANS éclair.

Ce qui change par rapport à la passe 1 :
  · plus d'éclair (refusé) — remplacé par une **gorge orange** de 1,6 mm et une LED d'état ;
  · la **vraie interface** du moniteur est plaquée sur la dalle (capture 480×272 du simulateur,
    émissive) : on juge une coque avec son écran, pas avec un trou noir ;
  · **USB-C** chanfreiné sur le flanc, **LED** dans le menton, **peids** et passage de câble ;
  · la coque est **inclinée à 15°** sur son socle : sans ça, ça reste une boîte posée à plat.

    blender -b --factory-startup --python gen_boitier2.py -- <variante> <sortie.png> [ech]
"""
import bpy
import bmesh
import math
import sys
from mathutils import Vector

PCB_W, PCB_H = 123.0, 74.0
MOD_W, MOD_H = 105.5, 67.2
ACT_W, ACT_H = 95.04, 53.86
CLR, WALL = 1.5, 2.4
W = PCB_W + 2 * (CLR + WALL)          # 130,8
H = PCB_H + 2 * (CLR + WALL)          # 81,8
CHIN = 8.0                            # menton : gorge orange + LED
HF = H + CHIN
D_BODY = 21.0
D_LID = 4.0
TILT = 70.0                            # 70° depuis l'horizontale : un moniteur se regarde,
                                       # il ne se lit pas à plat (20° de renversement)

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
VAR = int(argv[0]) if argv else 1
OUT = argv[1] if len(argv) > 1 else "/tmp/b2.png"
SAMP = int(argv[2]) if len(argv) > 2 else 96
ECRAN = argv[3] if len(argv) > 3 else "/tmp/cap/02_reseau_activite.png"

BODY_COL = (0.075, 0.080, 0.090)
ACCENT = (1.0, 0.30, 0.02)

# variantes : traitement des arêtes et des flancs
if VAR == 1:
    CHAMFER, CORNER, NSLAT = 3.0, 7.0, 4
elif VAR == 2:
    CHAMFER, CORNER, NSLAT = 1.5, 9.0, 0
else:
    CHAMFER, CORNER, NSLAT = 2.2, 5.0, 7

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
sc = bpy.context.scene


def aim(o, target=(0, 0, 0)):
    d = Vector(target) - o.location
    o.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()


def cube(name, size, loc):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc)
    o = bpy.context.object
    o.name = name
    o.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return o


def bevel(o, width, segments=1):
    m = o.modifiers.new("bev", "BEVEL")
    m.width = width
    m.segments = segments
    m.limit_method = "ANGLE"
    m.angle_limit = math.radians(30)
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


def mat(name, base, rough=0.6, metallic=0.0, emit=None, emit_str=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*base, 1.0)
    b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = metallic
    if emit:
        b.inputs["Emission Color"].default_value = (*emit, 1.0)
        b.inputs["Emission Strength"].default_value = emit_str
    return m


M_BODY = mat("corps", BODY_COL, 0.58)
M_ACCENT = mat("accent", ACCENT, 0.35, 0.3, emit=ACCENT, emit_str=0.7)
M_LED = mat("led", (0.1, 1.0, 0.35), 0.3, 0.0, emit=(0.1, 1.0, 0.35), emit_str=6.0)
M_GLASS = mat("vitre", (0.006, 0.007, 0.010), 0.08, 0.0)
M_PORT = mat("port", (0.02, 0.02, 0.022), 0.85)

# ── Coque ─────────────────────────────────────────────────────────────────
body = cube("coque", (W, HF, D_BODY), (0, 0, D_BODY / 2))
bevel(body, CHAMFER)

for sx in (-1, 1):
    for sy in (-1, 1):
        c = cube("cut", (CORNER * 2, CORNER * 2, D_BODY + 8),
                 (sx * (W / 2 - CORNER / 2), sy * (HF / 2 - CORNER / 2), D_BODY / 2))
        c.rotation_euler[2] = math.radians(45)
        bpy.context.view_layer.objects.active = c
        bpy.ops.object.transform_apply(rotation=True)
        boolean(body, c)

# logement de l'écran + lèvre
boolean(body, cube("ouv", (MOD_W + 0.6, MOD_H + 0.6, D_BODY), (0, CHIN / 2, D_BODY / 2 + 1.0)))

# flancs : ailettes de ventilation
if NSLAT:
    for side in (-1, 1):
        for k in range(NSLAT):
            y = -16 + k * (32 / max(1, NSLAT - 1)) if NSLAT > 1 else 0
            boolean(body, cube("slat", (W * 0.46, 2.0, 6.5), (side * (W / 2 - W * 0.2), y, D_BODY * 0.45)))

# USB-C sur le flanc droit, centré verticalement (contrainte du PCB)
port = cube("usbc", (3.6, 9.2, 3.6), (W / 2 - 1.0, 4.0, D_BODY / 2))
bevel(port, 0.9, 2)
boolean(body, port)

# gorge orange dans le menton (remplace l'éclair) : creusée puis comblée
gorge_y = -HF / 2 + 4.4
boolean(body, cube("gcut", (W - 26.0, 1.7, 1.6), (0, gorge_y, D_BODY - 0.7)))
gorge = cube("gorge", (W - 26.0, 1.1, 0.9), (0, gorge_y, D_BODY - 0.55))
gorge.data.materials.append(M_ACCENT)

# LED d'état à gauche, dans le menton
led_r = 1.3
boolean(body, cube("lcut", (2.6, 2.6, 2.0), (-W / 2 + 11.0, gorge_y, D_BODY - 0.9)))
led = cube("led", (2.2, 2.2, 1.2), (-W / 2 + 11.0, gorge_y, D_BODY - 0.8))
led.data.materials.append(M_LED)

body.data.materials.append(M_BODY)

# ── Dalle + vraie interface ───────────────────────────────────────────────
glass = cube("vitre", (ACT_W + 3.0, ACT_H + 3.0, 0.6), (0, CHIN / 2, D_BODY - 1.5))
glass.data.materials.append(M_GLASS)

# plan (UV 0→1) porteur de la capture réelle du simulateur, en émission
bpy.ops.mesh.primitive_plane_add(size=1, location=(0, CHIN / 2, D_BODY - 1.15))
ecr = bpy.context.object
ecr.name = "ecran"
ecr.scale = (ACT_W, ACT_H, 1)
bpy.ops.object.transform_apply(scale=True)
m = bpy.data.materials.new("ecran")
m.use_nodes = True
nt = m.node_tree
for n in list(nt.nodes):
    if n.type == "BSDF_PRINCIPLED":
        nt.nodes.remove(n)
out = [n for n in nt.nodes if n.type == "OUTPUT_MATERIAL"][0]
tex = nt.nodes.new("ShaderNodeTexImage")
img = bpy.data.images.load(ECRAN)
img.colorspace_settings.name = "sRGB"
tex.image = img
tex.interpolation = "Cubic"
em = nt.nodes.new("ShaderNodeEmission")
em.inputs["Strength"].default_value = 1.15
nt.links.new(tex.outputs["Color"], em.inputs["Color"])
nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
ecr.data.materials.append(m)

# ── Dos rainuré ───────────────────────────────────────────────────────────
lid = cube("dos", (W - 2.0, HF - 2.0, D_LID), (0, 0, -D_LID / 2))
bevel(lid, 1.0)
for i in range(-3, 4):
    boolean(lid, cube("rain", (W - 24.0, 3.2, D_LID + 3), (0, i * 9.0, -D_LID / 2)))
lid.data.materials.append(M_BODY)

# ── Ensemble : inclinaison 15° autour de l'arête avant du socle ────────────
bpy.ops.object.empty_add(location=(0, 0, 0))
pivot = bpy.context.object
pivot.name = "pivot"
for o in (body, glass, ecr, gorge, led, lid):
    o.parent = pivot
    o.matrix_parent_inverse = pivot.matrix_world.inverted()
pivot.rotation_euler[0] = math.radians(TILT)

# socle : coin plein sous l'ensemble incliné, avec passage de câble
bpy.context.view_layer.update()
zmin = min((o.matrix_world @ Vector(c)).z for o in (body, lid) for c in o.bound_box)
# coque bien plus haute qu'épaisse : le piètement porte derrière, pas dessous
base = cube("socle", (104, 74, 7.0), (0, 10.0, -3.5))
bevel(base, 1.6)
boolean(base, cube("cable", (16, 12, 8), (0, 10.0 - 37.0 + 4.0, -3.5)))
base.data.materials.append(M_BODY)
col = cube("col", (52, 18, 58), (0, 34.0, 26.0))
bevel(col, 2.0)
col.rotation_euler[0] = math.radians(-18)
bpy.context.view_layer.objects.active = col
bpy.ops.object.transform_apply(rotation=True)
col.data.materials.append(M_BODY)

# petits pieds
for sx in (-1, 1):
    for sy in (-1, 1):
        f = cube("pied", (9, 9, 3.0), (sx * 42.0, 10.0 + sy * 28.0, -8.0))
        bevel(f, 0.8)
        f.data.materials.append(M_PORT)

# ── Échelle : mm → m (sinon la coque mesure 130 m et la caméra est dedans) ─
for o in bpy.data.objects:
    if o.type == "MESH":
        o.scale = (0.001, 0.001, 0.001)
        o.location = o.location * 0.001
bpy.ops.object.select_all(action="SELECT")
bpy.context.view_layer.objects.active = body
bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
print("[échelle] coque %.3f m de large, inclinaison %.0f°" % (W / 1000.0, TILT))

# Boîte englobante de l'assemblage : sert au cadrage et à la visée des lampes.
_pts = []
for _o in bpy.data.objects:
    if _o.type == "MESH" and _o.name != "sol":
        _pts += [_o.matrix_world @ Vector(c) for c in _o.bound_box]
_lo = Vector((min(p.x for p in _pts), min(p.y for p in _pts), min(p.z for p in _pts)))
_hi = Vector((max(p.x for p in _pts), max(p.y for p in _pts), max(p.z for p in _pts)))
_centre = (_lo + _hi) / 2
_taille = _hi - _lo


# ── Studio ────────────────────────────────────────────────────────────────
world = bpy.data.worlds.new("w")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.05, 0.052, 0.06, 1)
sc.world = world

bpy.ops.object.light_add(type="AREA", location=(-0.30, -0.34, 0.32))
key = bpy.context.object
key.data.energy = 5.5
key.data.size = 0.22
key.data.color = (1.0, 0.94, 0.88)
aim(key, (_centre.x, _centre.y, _centre.z))

bpy.ops.object.light_add(type="AREA", location=(0.36, -0.10, 0.10))
rim = bpy.context.object
rim.data.energy = 1.5
rim.data.size = 0.12
rim.data.color = (1.0, 0.35, 0.08)
aim(rim, (_centre.x, _centre.y, _centre.z))

bpy.ops.object.light_add(type="AREA", location=(0.05, 0.40, 0.20))
back = bpy.context.object
back.data.energy = 2.2
back.data.size = 0.16
back.data.color = (0.55, 0.7, 1.0)
aim(back, (_centre.x, _centre.y, _centre.z))

bpy.ops.object.light_add(type="AREA", location=(-0.38, 0.24, 0.14))
fill = bpy.context.object
fill.data.energy = 2.4
fill.data.size = 0.20
fill.data.color = (0.8, 0.86, 1.0)
aim(fill, (_centre.x, _centre.y, _centre.z))

# Cadrage CALCULÉ sur la boîte englobante de l'assemblage (un cadrage deviné se rate) :
# on prend le centre et on recule juste ce qu'il faut pour que le sujet remplisse le cadre.
LENS, SENSOR = 62.0, 36.0
_hfov = math.atan((SENSOR / 2) / LENS)                  # demi-champ horizontal
_vfov = math.atan((SENSOR * 900 / 1400 / 2) / LENS)     # capteur "auto" : hauteur = 36 × 900/1400
_dist = max(_taille.x / (2 * math.tan(_hfov)), _taille.z / (2 * math.tan(_vfov))) * 1.18
_azim, _elev = math.radians(34), math.radians(19)
_cdir = Vector((math.sin(_azim) * math.cos(_elev), -math.cos(_azim) * math.cos(_elev), math.sin(_elev)))
bpy.ops.object.camera_add(location=_centre + _cdir * _dist)
cam = bpy.context.object
cam.data.lens = LENS
cam.data.sensor_width = SENSOR
sc.camera = cam
cam.rotation_euler = (_centre - cam.location).to_track_quat("-Z", "Y").to_euler()
print("[cadre] sujet %.3f × %.3f × %.3f m · caméra à %.2f m · vise %.3f, %.3f, %.3f"
      % (_taille.x, _taille.y, _taille.z, _dist, _centre.x, _centre.y, _centre.z))

cube("sol", (2.4, 2.4, 0.004), (0, 0, -0.016)).data.materials.append(mat("solmat", (0.05, 0.052, 0.058), 0.5))
_mur = cube("mur", (2.4, 0.006, 1.6), (0, 0.9, 0.6))
_mur.data.materials.append(mat("murm", (0.030, 0.032, 0.038), 0.9))

sc.render.engine = "CYCLES"
prefs = bpy.context.preferences.addons["cycles"].preferences
for t in ("OPTIX", "CUDA"):
    try:
        prefs.compute_device_type = t
        prefs.refresh_devices()
        prefs.get_devices()
        if [dd for dd in prefs.devices if dd.type != "CPU"]:
            for dd in prefs.devices:
                dd.use = dd.type != "CPU"
            sc.cycles.device = "GPU"
            print(f"[gpu] {t}")
            break
    except Exception as e:
        print(f"[gpu] {t} : {e}")
else:
    sc.cycles.device = "CPU"

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
sc.render.filepath = OUT
sc.render.image_settings.file_format = "PNG"

bpy.ops.render.render(write_still=True)
print(f"[rendu] {OUT}")
