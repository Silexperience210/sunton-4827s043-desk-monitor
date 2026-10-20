"""Boîtier Sunton 4827S043 — passe 3 : assemblage construit DEBOUT, puis incliné.

Les deux passes précédentes construisaient la coque à plat (écran vers +Z) puis la redressaient :
le piètement se retrouvait à travers l'écran. Ici tout est coté dans le repère PRODUIT — écran vers
-Y, pied vers le bas — puis l'ensemble est incliné de 20° autour de son arête avant basse, et le
socle se déduit de la boîte englobante mesurée, jamais devinée.

    blender -b --factory-startup --python gen_boitier3.py -- <variante> <sortie.png> [ech]
"""
import bpy
import math
import sys
from mathutils import Vector

PCB_W, PCB_H = 123.0, 74.0
MOD_W, MOD_H = 105.5, 67.2
ACT_W, ACT_H = 95.04, 53.86
WALL, CLR = 2.4, 1.5
W = PCB_W + 2 * (CLR + WALL)          # 130,8
H = PCB_H + 2 * (CLR + WALL)          # 81,8
CHIN = 9.0                             # menton (gorge + LED)
HF = H + CHIN                          # 90,8
D = 21.0                               # profondeur de coque
D_LID = 4.0
TILT = 20.0                            # renversement depuis la verticale

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
VAR = int(argv[0]) if argv else 1
OUT = argv[1] if len(argv) > 1 else "/tmp/b3.png"
SAMP = int(argv[2]) if len(argv) > 2 else 96
ECRAN = argv[3] if len(argv) > 3 else "/tmp/cap/02_reseau_activite.png"

BODY_COL = (0.072, 0.077, 0.086)
ACCENT = (1.0, 0.30, 0.02)
CHAMFER, CORNER, NSLAT = (2.6, 7.0, 5) if VAR == 1 else ((1.4, 8.0, 0) if VAR == 2 else (2.0, 5.0, 8))

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


def bevel(o, w, seg=1):
    m = o.modifiers.new("b", "BEVEL")
    m.width, m.segments = w, seg
    m.limit_method = "ANGLE"
    m.angle_limit = math.radians(30)
    bpy.context.view_layer.objects.active = o
    bpy.ops.object.modifier_apply(modifier=m.name)


def diff(t, c):
    m = t.modifiers.new("d", "BOOLEAN")
    m.operation = "DIFFERENCE"
    m.solver = "EXACT"
    m.object = c
    bpy.context.view_layer.objects.active = t
    bpy.ops.object.modifier_apply(modifier=m.name)
    bpy.data.objects.remove(c, do_unlink=True)


def mat(name, base, rough=0.6, metallic=0.0, emit=None, es=0.0):
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


M_BODY = mat("corps", BODY_COL, 0.55)
M_ACCENT = mat("accent", ACCENT, 0.35, 0.3, emit=ACCENT, es=0.8)
M_LED = mat("led", (0.1, 1.0, 0.35), 0.3, 0, emit=(0.1, 1.0, 0.35), es=6.0)
M_GLASS = mat("vitre", (0.005, 0.006, 0.008), 0.28)
M_DARK = mat("sombre", (0.02, 0.021, 0.024), 0.85)

# ── Repère produit : écran vers -Y, socle vers -Z ──────────────────────────
# coque : y de 0 (façade) à +D (arrière), z de 0 (bas) à HF (haut)
body = box("coque", (W, D, HF), (0, D / 2, HF / 2))
bevel(body, CHAMFER)

# coins coupés : coupe verticale, dans le plan XY de la façade
for sx in (-1, 1):
    for sz in (0, 1):
        c = box("cut", (CORNER * 2.4, D + 8, CORNER * 2.4),
                (sx * (W / 2), D / 2, 0.0 if sz == 0 else HF))
        c.rotation_euler[1] = math.radians(45)
        bpy.context.view_layer.objects.active = c
        bpy.ops.object.transform_apply(rotation=True)
        diff(body, c)

# logement de l'écran (traversant en Y) : la lèvre couvre le chant du module
screen_cz = CHIN + H / 2
diff(body, box("ouv", (MOD_W + 0.6, D + 4, MOD_H + 0.6), (0, D / 2 - 1.0, screen_cz)))

# ailettes de ventilation sur les flancs
if NSLAT:
    for side in (-1, 1):
        for k in range(NSLAT):
            z = HF * 0.35 + k * (HF * 0.30 / max(1, NSLAT - 1)) if NSLAT > 1 else HF * 0.45
            diff(body, box("slat", (W * 0.5, 2.0, 6.0), (side * (W / 2), D * 0.72, z)))

# USB-C sur le flanc gauche, à mi-hauteur de la coque
port = box("usbc", (3.4, 9.0, 3.4), (-W / 2 + 0.8, D * 0.6, HF * 0.30))
bevel(port, 0.8, 2)
diff(body, port)

# gorge orange + LED dans le menton (façade à y = 0)
gy = -0.0
diff(body, box("gc", (W - 30.0, 1.6, 1.5), (0, gy + 0.8, CHIN * 0.55)))
gorge = box("gorge", (W - 30.0, 1.0, 0.9), (0, gy + 0.55, CHIN * 0.55))
gorge.data.materials.append(M_ACCENT)
diff(body, box("lc", (2.6, 1.6, 2.6), (-W / 2 + 12.0, gy + 0.8, CHIN * 0.55)))
led = box("led", (2.1, 1.0, 2.1), (-W / 2 + 12.0, gy + 0.5, CHIN * 0.55))
led.data.materials.append(M_LED)

body.data.materials.append(M_BODY)

# vitre + interface réelle, plaquées en retrait de 1,2 mm
glass = box("vitre", (MOD_W - 0.4, 0.6, MOD_H - 0.4), (0, 1.4, screen_cz))
glass.data.materials.append(M_GLASS)

bpy.ops.mesh.primitive_plane_add(size=1, location=(0, 1.05, screen_cz), rotation=(math.radians(90), 0, 0))
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
tex.image = bpy.data.images.load(ECRAN)
tex.image.colorspace_settings.name = "sRGB"
em = nt.nodes.new("ShaderNodeEmission")
em.inputs["Strength"].default_value = 1.1
nt.links.new(tex.outputs["Color"], em.inputs["Color"])
nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
ecr.data.materials.append(m)

# ── Inclinaison de 20° autour de l'arête basse avant (y=0, z=0) ────────────
bpy.ops.object.empty_add(location=(0, 0, 0))
pivot = bpy.context.object
for o in (body, glass, ecr, gorge, led):
    o.parent = pivot
    o.matrix_parent_inverse = pivot.matrix_world.inverted()
pivot.rotation_euler[0] = math.radians(TILT)      # le haut part vers +Y (vers l'arrière)
bpy.context.view_layer.update()

# dos rainuré, construit dans le repère produit puis incliné avec le reste
lid = box("dos", (W - 2.0, D_LID, HF - 2.0), (0, D + D_LID / 2, HF / 2))
bevel(lid, 1.0)
for i in range(-3, 4):
    diff(lid, box("rain", (W - 26.0, D_LID + 3, 3.0), (0, D + D_LID / 2, HF / 2 + i * 9.0)))
lid.data.materials.append(M_BODY)
lid.parent = pivot
lid.matrix_parent_inverse = pivot.matrix_world.inverted()
bpy.context.view_layer.update()

# ── Piètement déduit de la géométrie mesurée ──────────────────────────────
pts = []
for o in (body, lid):
    pts += [o.matrix_world @ Vector(c) for c in o.bound_box]
zmin = min(p.z for p in pts)
ymax = max(p.y for p in pts)

# plaque de base + colonne inclinée qui rejoint le dos de la coque
base = box("socle", (W * 0.78, 96.0, 11.0), (0, ymax * 0.40, zmin + 5.5))
bevel(base, 1.8)
diff(base, box("cable", (16.0, 14.0, 12.0), (0, ymax * 0.40 + 48.0 - 4.0, zmin + 5.5)))
base.data.materials.append(M_BODY)

col = box("colonne", (72.0, 24.0, 92.0), (0, ymax * 0.58, zmin + 11.0 + 40.0))
bevel(col, 2.0)
col.rotation_euler[0] = math.radians(TILT)
bpy.context.view_layer.objects.active = col
bpy.ops.object.transform_apply(rotation=True)
col.data.materials.append(M_BODY)

for sx in (-1, 1):
    for sy in (-1, 1):
        f = box("pied", (10, 10, 3.5), (sx * W * 0.33, ymax * 0.40 + sy * 38.0, zmin - 0.5))
        bevel(f, 0.9)
        f.data.materials.append(M_DARK)

# ── mm → m ────────────────────────────────────────────────────────────────
for o in bpy.data.objects:
    if o.type == "MESH":
        o.scale = (0.001,) * 3
        o.location = o.location * 0.001
bpy.ops.object.select_all(action="SELECT")
bpy.context.view_layer.objects.active = body
bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

# ── Cadrage calculé sur la boîte englobante ───────────────────────────────
pts = []
for o in bpy.data.objects:
    if o.type == "MESH":
        pts += [o.matrix_world @ Vector(c) for c in o.bound_box]
lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
ctr = (lo + hi) / 2
taille = hi - lo
LENS, SENS = 62.0, 36.0
hf = math.atan((SENS / 2) / LENS)
vf = math.atan((SENS * 900 / 1400 / 2) / LENS)
dist = max(taille.x / (2 * math.tan(hf)), taille.z / (2 * math.tan(vf))) * 1.16

azim, elev = math.radians(34), math.radians(16)
cdir = Vector((math.sin(azim) * math.cos(elev), -math.cos(azim) * math.cos(elev), math.sin(elev)))
bpy.ops.object.camera_add(location=ctr + cdir * dist)
cam = bpy.context.object
cam.data.lens = LENS
cam.data.sensor_width = SENS
sc.camera = cam
cam.rotation_euler = (ctr - cam.location).to_track_quat("-Z", "Y").to_euler()
print("[cadre] %.3f × %.3f × %.3f m · caméra %.2f m · centre %.3f %.3f %.3f"
      % (taille.x, taille.y, taille.z, dist, ctr.x, ctr.y, ctr.z))

# ── Studio ────────────────────────────────────────────────────────────────
world = bpy.data.worlds.new("w")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.05, 0.052, 0.06, 1)
sc.world = world

for loc, energy, size, col, tgt in (
        ((-0.32, -0.34, 0.34), 6.0, 0.24, (1.0, 0.94, 0.88), ctr),
        ((0.40, 0.10, 0.10), 0.9, 0.12, (1.0, 0.35, 0.08), ctr),
        ((0.06, 0.42, 0.22), 2.4, 0.18, (0.55, 0.7, 1.0), ctr),
        ((-0.40, 0.26, 0.16), 2.6, 0.20, (0.8, 0.86, 1.0), ctr)):
    bpy.ops.object.light_add(type="AREA", location=loc)
    L = bpy.context.object
    L.data.energy = energy
    L.data.size = size
    L.data.color = col
    aim(L, tgt)

box("sol", (2.4, 2.4, 0.004), (0, 0.1, lo.z - 0.002)).data.materials.append(mat("sol", (0.045, 0.047, 0.052), 0.45))
box("mur", (2.4, 0.006, 1.6), (0, 0.95, 0.55)).data.materials.append(mat("mur", (0.028, 0.030, 0.036), 0.9))

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
sc.view_settings.exposure = -0.2
sc.render.resolution_x, sc.render.resolution_y = 1400, 900
sc.render.filepath = OUT
sc.render.image_settings.file_format = "PNG"
bpy.ops.render.render(write_still=True)
print(f"[rendu] {OUT}")
