"""Boîtier Sunton 4827S043 — passe 4 : arrondi, affiné.

Retours du propriétaire : « massif », « pourquoi pas les bords arrondis ». Donc :
  · plus de coins coupés à 45° : un congé généreux (r 6 mm, 8 segments) sur toute la coque ;
  · profondeur 16 mm au lieu de 21, parois 2,0, menton 6 mm — la masse était dans l'épaisseur ;
  · façade légèrement rentrante (dépouille) pour que le cadre se lise comme un cadre, pas un bloc ;
  · piètement fondu dans la coque : un pied trapézoïdal arrondi, pas une colonne invisible ;
  · inclinaison 12° (un moniteur de bureau, pas une console de voiture).

    blender -b --factory-startup --python gen_boitier4.py -- <variante> <sortie.png> [ech]
"""
import bpy
import math
import sys
from mathutils import Vector

PCB_W, PCB_H = 123.0, 74.0
MOD_W, MOD_H = 105.5, 67.2
ACT_W, ACT_H = 95.04, 53.86
WALL, CLR = 2.0, 1.2
W = PCB_W + 2 * (CLR + WALL)          # 129,4
H = PCB_H + 2 * (CLR + WALL)          # 80,4
CHIN = 6.0
HF = H + CHIN                          # 86,4
D = 16.0
DRAFT = 2.5                            # la façade rentre de 2,5 mm : dépouille
TILT = 9.0

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
VAR = int(argv[0]) if argv else 1
OUT = argv[1] if len(argv) > 1 else "/tmp/b4.png"
SAMP = int(argv[2]) if len(argv) > 2 else 96
ECRAN = argv[3] if len(argv) > 3 else "/tmp/cap/02_reseau_activite.png"

BODY_COL = (0.075, 0.080, 0.090)
ACCENT = (1.0, 0.30, 0.02)
R = (6.0, 8, 4) if VAR == 1 else ((4.0, 6, 3) if VAR == 2 else (8.0, 10, 6))

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


def bevel(o, w, seg=6):
    m = o.modifiers.new("b", "BEVEL")
    m.width, m.segments = w, seg
    m.limit_method = "ANGLE"
    m.angle_limit = math.radians(30)
    m.profile = 0.5
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


def uni(t, c):
    m = t.modifiers.new("u", "BOOLEAN")
    m.operation = "UNION"
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


M_BODY = mat("corps", BODY_COL, 0.52)
M_ACCENT = mat("accent", ACCENT, 0.35, 0.3, emit=ACCENT, es=0.7)
M_LED = mat("led", (0.1, 1.0, 0.35), 0.3, 0, emit=(0.1, 1.0, 0.35), es=5.0)
M_GLASS = mat("vitre", (0.005, 0.006, 0.008), 0.30)
M_DARK = mat("sombre", (0.02, 0.021, 0.024), 0.85)

# ── Coque arrondie ────────────────────────────────────────────────────────
body = box("coque", (W, D, HF), (0, D / 2, HF / 2))

# logement d'écran AVANT le congé : le congé adoucit aussi la lèvre intérieure
screen_cz = CHIN + H / 2
diff(body, box("ouv", (MOD_W + 0.6, D + 4, MOD_H + 0.6), (0, D / 2 - 1.0, screen_cz)))

# Façade en deux niveaux : la PLAQUE avant ne fait que la largeur du module (110 × 70), la coque
# profonde reste derrière. C'est ce décrochement qui fait lire un cadre fin malgré un PCB de 123 mm.
plate = box("plaque", (MOD_W + 15.0, 4.5, MOD_H + 15.0), (0, -1.0, screen_cz))
# l'ouverture traverse la plaque ET la coque
diff(plate, box("ouvp", (MOD_W + 0.6, 20, MOD_H + 0.6), (0, -3.0, screen_cz)))
bevel(plate, 3.0, 6)
plate.data.materials.append(M_BODY)
uni(body, plate)

bevel(body, R[0], R[1])

# ailettes de ventilation sur les flancs arrière
for side in (-1, 1):
    for k in range(5):
        z = HF * 0.34 + k * (HF * 0.26 / 4)
        diff(body, box("slat", (W * 0.4, 2.0, 5.0), (side * (W / 2 - W * 0.18), D * 0.78, z)))

# USB-C flanc gauche
port = box("usbc", (3.2, 8.8, 3.2), (-W / 2 + 0.6, D * 0.55, HF * 0.28))
bevel(port, 0.7, 3)
diff(body, port)

# gorge orange + LED dans le menton
gy = CHIN * 0.55
diff(body, box("gc", (W - 34.0, 1.5, 1.3), (0, 0.75, gy)))
gorge = box("gorge", (W - 34.0, 0.9, 0.8), (0, 0.5, gy))
gorge.data.materials.append(M_ACCENT)
diff(body, box("lc", (2.4, 1.5, 2.4), (-W / 2 + 13.0, 0.75, gy)))
led = box("led", (1.9, 0.9, 1.9), (-W / 2 + 13.0, 0.45, gy))
led.data.materials.append(M_LED)
body.data.materials.append(M_BODY)

# vitre (taille du module) + interface réelle
glass = box("vitre", (MOD_W - 0.4, 0.6, MOD_H - 0.4), (0, 1.5, screen_cz))
glass.data.materials.append(M_GLASS)
bpy.ops.mesh.primitive_plane_add(size=1, location=(0, 1.15, screen_cz), rotation=(math.radians(90), 0, 0))
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
em.inputs["Strength"].default_value = 1.05
nt.links.new(tex.outputs["Color"], em.inputs["Color"])
nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
ecr.data.materials.append(m)

# ── Dos arrondi, rainures fines ───────────────────────────────────────────
lid = box("dos", (W - 3.0, 4.0, HF - 3.0), (0, D + 2.0, HF / 2))
bevel(lid, 3.0, 6)
for i in range(-3, 4):
    diff(lid, box("rain", (W - 30.0, 6.0, 2.2), (0, D + 2.0, HF / 2 + i * 8.0)))
lid.data.materials.append(M_BODY)

# ── Piètement fondu : trapèze arrondi sous l'arrière de la coque ───────────
foot = box("pied", (W * 0.64, 46.0, 5.0), (0, D * 1.05, -2.5))
bevel(foot, 2.4, 6)
heel = box("talon", (W * 0.40, 16.0, 34.0), (0, D * 1.25, -20.0))
bevel(heel, 4.0, 6)
heel.data.materials.append(M_BODY)
uni(foot, heel)
foot.data.materials.append(M_BODY)
diff(foot, box("cable", (18.0, 14.0, 10.0), (0, D * 0.9 + 39.0 - 3.0, -14.0)))

# ── Inclinaison douce de l'ensemble coque+dos autour de l'arête basse ─────
bpy.ops.object.empty_add(location=(0, 0, 0))
pivot = bpy.context.object
for o in (body, glass, ecr, gorge, led, lid):
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
bpy.context.view_layer.objects.active = body
bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

# ── Cadrage calculé ───────────────────────────────────────────────────────
pts = []
for o in bpy.data.objects:
    if o.type == "MESH":
        pts += [o.matrix_world @ Vector(c) for c in o.bound_box]
lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
ctr = (lo + hi) / 2
taille = hi - lo
LENS, SENS = 68.0, 36.0
hf = math.atan((SENS / 2) / LENS)
vf = math.atan((SENS * 900 / 1400 / 2) / LENS)
dist = max(taille.x / (2 * math.tan(hf)), taille.z / (2 * math.tan(vf))) * 1.14
azim, elev = math.radians(30), math.radians(28)
cdir = Vector((math.sin(azim) * math.cos(elev), -math.cos(azim) * math.cos(elev), math.sin(elev)))
bpy.ops.object.camera_add(location=ctr + cdir * dist)
cam = bpy.context.object
cam.data.lens = LENS
cam.data.sensor_width = SENS
sc.camera = cam
cam.rotation_euler = (ctr - cam.location).to_track_quat("-Z", "Y").to_euler()
print("[cadre] %.3f × %.3f × %.3f m · cam %.2f m" % (taille.x, taille.y, taille.z, dist))

# ── Contexte d'échelle : sans repère, un objet de 13 cm se lit comme un écran de 60 cm ──
M_BOIS = mat("bois", (0.085, 0.055, 0.035), 0.55)
M_PAPIER = mat("papier", (0.62, 0.60, 0.56), 0.75)
M_CERAM = mat("ceramique", (0.55, 0.56, 0.58), 0.35)

desk = box("bureau", (0.90, 0.52, 0.018), (0, 0.10, -0.026))
bevel(desk, 0.004, 4)
desk.data.materials.append(M_BOIS)

carnet = box("carnet", (0.148, 0.210, 0.012), (-0.135, 0.02, -0.005))
bevel(carnet, 0.0015, 3)
carnet.rotation_euler[2] = math.radians(9)
carnet.data.materials.append(M_PAPIER)

tasse = bpy.data.objects.get("tasse")
bpy.ops.mesh.primitive_cylinder_add(vertices=48, radius=0.041, depth=0.092, location=(0.128, -0.055, 0.046))
tasse = bpy.context.object
tasse.name = "tasse"
bevel(tasse, 0.002, 3)
tasse.data.materials.append(M_CERAM)

# ── Studio ────────────────────────────────────────────────────────────────
world = bpy.data.worlds.new("w")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.055, 0.057, 0.065, 1)
sc.world = world
for loc, en, sz, col in (((-0.32, -0.34, 0.34), 5.5, 0.26, (1.0, 0.95, 0.90)),
                         ((0.40, 0.10, 0.12), 0.45, 0.10, (0.75, 0.82, 1.0)),
                         ((0.06, 0.42, 0.22), 2.0, 0.18, (0.55, 0.7, 1.0)),
                         ((-0.40, 0.26, 0.16), 2.2, 0.20, (0.8, 0.86, 1.0))):
    bpy.ops.object.light_add(type="AREA", location=loc)
    L = bpy.context.object
    L.data.energy, L.data.size, L.data.color = en, sz, col
    aim(L, ctr)

box("sol", (2.4, 2.4, 0.004), (0, 0.1, -0.03)).data.materials.append(mat("sol", (0.02, 0.021, 0.025), 0.7))
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
sc.view_settings.exposure = -0.15
sc.render.resolution_x, sc.render.resolution_y = 1400, 900
sc.render.filepath = OUT
sc.render.image_settings.file_format = "PNG"
bpy.ops.render.render(write_still=True)
print(f"[rendu] {OUT}")
