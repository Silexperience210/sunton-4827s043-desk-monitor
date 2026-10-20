"""Boîtier Sunton 4827S043 — passe 5 : SLIM et SIMPLE, autour d'un modèle RÉEL de la carte.

Deux livrables dans ce script :
  1. le **modèle de référence de la carte** (PCB 123 × 74 × 1,6 + dalle 105,5 × 67,2 + connecteur +
     quatre trous Ø3,2), exporté en STL — c'est lui qui fait foi pour toutes les cavités ;
  2. le **boîtier** : 9,5 mm d'épaisseur totale (4 de façade + 5,5 de dos), parois 1,6, jeu 0,5,
     plus de menton, plus de gorge : une seule ligne orange en creux, renversé de 22°.

    blender -b --factory-startup --python gen_boitier5.py -- <variante> <sortie.png> [ech]
"""
import bpy
import math
import os
import sys
from mathutils import Vector

PCB_W, PCB_H, PCB_T = 123.0, 74.0, 1.6
MOD_W, MOD_H, MOD_T = 105.5, 67.2, 3.0
ACT_W, ACT_H = 95.04, 53.86
HOLE_D = 3.2
HOLE_DX, HOLE_DZ = 105.5, 67.2          # entraxe des quatre trous
WALL, CLR = 1.6, 0.5
W = PCB_W + 2 * (CLR + WALL)            # 127,2
H = PCB_H + 2 * (CLR + WALL)            # 78,2
D_FRONT, D_BACK = 4.0, 5.5
TILT = 22.0

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
VAR = int(argv[0]) if argv else 1
OUT = argv[1] if len(argv) > 1 else "/tmp/b5.png"
SAMP = int(argv[2]) if len(argv) > 2 else 96
ECRAN = argv[3] if len(argv) > 3 else "/tmp/cap/02_reseau_activite.png"
STL_REF = "/home/silex/sunton-4827s043-desk-monitor/case/stl/sunton-4827s043-carte.stl"

BODY_COL = (0.070, 0.074, 0.082)
ACCENT = (1.0, 0.30, 0.02)
R = (2.5, 6) if VAR == 1 else ((1.6, 5) if VAR == 2 else (4.0, 8))

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
sc = bpy.context.scene


def aim(o, t):
    o.rotation_euler = (Vector(t) - o.location).to_track_quat("-Z", "Y").to_euler()


def box(name, size, loc, rot=None):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc)
    o = bpy.context.object
    o.name = name
    o.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if rot:
        o.rotation_euler = rot
        bpy.ops.object.transform_apply(rotation=True)
    return o


def bevel(o, w, seg=4):
    m = o.modifiers.new("b", "BEVEL")
    m.width, m.segments = w, seg
    m.limit_method = "ANGLE"
    m.angle_limit = math.radians(30)
    bpy.context.view_layer.objects.active = o
    bpy.ops.object.modifier_apply(modifier=m.name)


def op(t, c, kind="DIFFERENCE"):
    m = t.modifiers.new("x", "BOOLEAN")
    m.operation = kind
    m.solver = "EXACT"
    m.object = c
    bpy.context.view_layer.objects.active = t
    bpy.ops.object.modifier_apply(modifier=m.name)
    bpy.data.objects.remove(c, do_unlink=True)


def mat(name, base, rough=0.55, metallic=0.0, emit=None, es=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*base, 1)
    b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = metallic
    if emit:
        b.inputs["Emission Color"].default_value = (*emit, 1)
        b.inputs["Emission Strength"].default_value = es
    return m


M_BODY = mat("corps", BODY_COL, 0.5)
M_ACCENT = mat("accent", ACCENT, 0.35, 0.3, emit=ACCENT, es=0.6)
M_GLASS = mat("vitre", (0.005, 0.006, 0.008), 0.3)
M_PCB = mat("pcb", (0.02, 0.10, 0.055), 0.75)
M_CHIP = mat("chip", (0.03, 0.03, 0.034), 0.8)
M_METAL = mat("metal", (0.55, 0.56, 0.58), 0.35, 0.9)

# ══ 1. MODÈLE DE RÉFÉRENCE DE LA CARTE ════════════════════════════════════
# Repère : façade de la dalle en y = 0, dos de la carte vers +y (comme le boîtier).
pcb = box("pcb", (PCB_W, PCB_T, PCB_H), (0, MOD_T + PCB_T / 2, PCB_H / 2))
bevel(pcb, 0.4, 2)
for sx in (-1, 1):
    for sz in (0, 1):
        op(pcb, box("hole", (HOLE_D, PCB_T + 2, HOLE_D),
                    (sx * HOLE_DX / 2, MOD_T + PCB_T / 2, (PCB_H / 2) + (-1 if sz == 0 else 1) * HOLE_DZ / 2)))
pcb.data.materials.append(M_PCB)

lcd = box("lcd", (MOD_W, MOD_T, MOD_H), (0, MOD_T / 2, PCB_H / 2))
bevel(lcd, 0.5, 2)
lcd.data.materials.append(M_GLASS)

# composants côté dos : module ESP32-S3 (18 × 25,5), régulateur, USB-C sur le chant gauche
esp = box("esp32", (18.0, 3.0, 25.5), (PCB_W / 2 - 22.0, MOD_T + PCB_T + 1.5, PCB_H / 2))
esp.data.materials.append(M_METAL)
reg = box("regul", (8.0, 1.2, 6.0), (-PCB_W / 2 + 26.0, MOD_T + PCB_T + 0.6, PCB_H / 2 - 12.0))
reg.data.materials.append(M_CHIP)
usb = box("usb", (9.0, 3.2, 3.4), (-PCB_W / 2 - 0.5, MOD_T + PCB_T / 2, PCB_H / 2))
usb.data.materials.append(M_METAL)

# export STL du modèle de référence (Blender 5 : ascii_format / apply_modifiers)
os.makedirs(os.path.dirname(STL_REF), exist_ok=True)
for o in (pcb, lcd, esp, reg, usb):
    o.select_set(True)
bpy.context.view_layer.objects.active = pcb
bpy.ops.wm.stl_export(filepath=STL_REF, export_selected_objects=True,
                      ascii_format=False, apply_modifiers=True)
print("[stl] %s" % STL_REF)
for o in bpy.data.objects:
    o.select_set(False)

# ══ 2. BOÎTIER ════════════════════════════════════════════════════════════
# façade 4 mm, dos 5,5 mm, ouverture = module + 0,6 ; écran à 1,2 mm sous la lèvre
total = D_FRONT + D_BACK
front = box("façade", (W, D_FRONT, H), (0, D_FRONT / 2, H / 2))
back = box("dos", (W, D_BACK, H), (0, D_FRONT + D_BACK / 2, H / 2))

# cavité intérieure (carte + dalle) dans la façade et le dos
op(back, box("cav2", (PCB_W + 2 * CLR, D_BACK - 1.6, PCB_H + 2 * CLR), (0, D_FRONT + D_BACK / 2 + 0.8, PCB_H / 2)))
# ouverture d'écran
op(front, box("ouv", (MOD_W + 0.6, D_FRONT + 4, MOD_H + 0.6), (0, D_FRONT / 2, PCB_H / 2)))

bevel(front, R[0], R[1])
bevel(back, R[0] + 0.8, R[1])

# USB-C : sortie sur le flanc gauche
op(front, box("usbc", (3.2, 9.4, 3.6), (-W / 2 + 0.6, D_FRONT / 2, PCB_H / 2 + 3.0)))
op(back, box("usbc2", (3.2, 9.4, 3.6), (-W / 2 + 0.6, D_FRONT + D_BACK / 2, PCB_H / 2 + 3.0)))

# une seule ligne orange en creux, en bas de la façade
op(front, box("gorgec", (W - 40.0, 1.2, 1.0), (0, 0.5, 5.0)))
gorge = box("gorge", (W - 40.0, 0.7, 0.6), (0, 0.35, 5.0))
gorge.data.materials.append(M_ACCENT)

# fentes de ventilation dans le dos (simples, alignées)
for k in range(4):
    op(back, box("slat", (W - 46.0, 6.0, 2.2), (0, D_FRONT + D_BACK / 2, H * 0.30 + k * 7.0)))

front.data.materials.append(M_BODY)
back.data.materials.append(M_BODY)

# écran : dalle réelle du module, avec l'interface
bpy.ops.mesh.primitive_plane_add(size=1, location=(0, -0.15, PCB_H / 2), rotation=(math.radians(90), 0, 0))
ecr = bpy.context.object
ecr.name = "ecran"
ecr.scale = (ACT_W, ACT_H, 1)
bpy.ops.object.transform_apply(scale=True)
m = bpy.data.materials.new("ecran")
m.use_nodes = True
nt = m.node_tree
bsdf = nt.nodes["Principled BSDF"]
tex = nt.nodes.new("ShaderNodeTexImage")
tex.image = bpy.data.images.load(ECRAN)
tex.image.colorspace_settings.name = "sRGB"
# Principled plutôt qu'un nœud Emission seul : il émet sur les deux faces, donc une inversion de
# normale ne peut plus éteindre l'écran (mesuré : 0 % de pixels orange avec le nœud Emission).
bsdf.inputs["Base Color"].default_value = (0, 0, 0, 1)
bsdf.inputs["Roughness"].default_value = 0.35
nt.links.new(tex.outputs["Color"], bsdf.inputs["Emission Color"])
bsdf.inputs["Emission Strength"].default_value = 1.4
ecr.data.materials.append(m)

# ── Piètement : semelle + talon, très bas ────────────────────────────────
foot = box("pied", (W * 0.58, 52.0, 4.0), (0, 30.0, -2.0))
bevel(foot, 1.8, 5)
heel = box("talon", (W * 0.34, 14.0, 30.0), (0, 52.0, -18.0))
bevel(heel, 3.0, 5)
op(foot, heel, "UNION")
foot.data.materials.append(M_BODY)

# ── Renversement de 22° ───────────────────────────────────────────────────
bpy.ops.object.empty_add(location=(0, 0, 0))
pivot = bpy.context.object
for o in (front, back, gorge, ecr, foot, pcb, lcd, esp, reg, usb):   # `heel` a été consommé par l'union
    o.parent = pivot
    o.matrix_parent_inverse = pivot.matrix_world.inverted()
pivot.rotation_euler[0] = math.radians(TILT)
bpy.context.view_layer.update()

# ── mm → m ────────────────────────────────────────────────────────────────
for o in bpy.data.objects:
    if o.type == "MESH":
        o.scale = (0.001,) * 3
        o.location = o.location * 0.001
bpy.ops.object.select_all(action="SELECT")
bpy.context.view_layer.objects.active = front
bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

# ── Cadrage ───────────────────────────────────────────────────────────────
pts = []
for o in bpy.data.objects:
    if o.type == "MESH" and o.name not in ("bureau", "carnet", "tasse", "sol", "mur"):
        pts += [o.matrix_world @ Vector(c) for c in o.bound_box]
lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
ctr = (lo + hi) / 2
taille = hi - lo
LENS, SENS = 72.0, 36.0
hfov = math.atan((SENS / 2) / LENS)
vfov = math.atan((SENS * 900 / 1400 / 2) / LENS)
dist = max(taille.x / (2 * math.tan(hfov)), taille.z / (2 * math.tan(vfov))) * 1.14
azim, elev = math.radians(32), math.radians(24)
cdir = Vector((math.sin(azim) * math.cos(elev), -math.cos(azim) * math.cos(elev), math.sin(elev)))
bpy.ops.object.camera_add(location=ctr + cdir * dist)
cam = bpy.context.object
cam.data.lens = LENS
cam.data.sensor_width = SENS
sc.camera = cam
cam.rotation_euler = (ctr - cam.location).to_track_quat("-Z", "Y").to_euler()
print("[cadre] objet %.3f × %.3f × %.3f m · cam %.2f m" % (taille.x, taille.y, taille.z, dist))

# ── Contexte d'échelle ────────────────────────────────────────────────────
M_BOIS = mat("bois", (0.075, 0.050, 0.032), 0.5)
desk = box("bureau", (0.90, 0.52, 0.018), (0, 0.10, -0.026))
bevel(desk, 0.004, 4)
desk.data.materials.append(M_BOIS)

# ── Studio ────────────────────────────────────────────────────────────────
world = bpy.data.worlds.new("w")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.05, 0.052, 0.06, 1)
sc.world = world
for loc, en, sz, col in (((-0.30, -0.32, 0.34), 5.0, 0.26, (1.0, 0.95, 0.90)),
                         ((0.40, 0.10, 0.12), 0.5, 0.10, (0.8, 0.86, 1.0)),
                         ((0.06, 0.42, 0.22), 1.8, 0.18, (0.55, 0.7, 1.0)),
                         ((-0.40, 0.26, 0.16), 2.0, 0.20, (0.85, 0.85, 0.9))):
    bpy.ops.object.light_add(type="AREA", location=loc)
    L = bpy.context.object
    L.data.energy, L.data.size, L.data.color = en, sz, col
    aim(L, ctr)

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
sc.view_settings.exposure = -0.15
sc.render.resolution_x, sc.render.resolution_y = 1400, 900
sc.render.filepath = OUT
sc.render.image_settings.file_format = "PNG"
bpy.ops.render.render(write_still=True)
print(f"[rendu] {OUT}")
