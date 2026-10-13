"""Desk stand for the Sunton ESP32-4827S043 (123 x 74 mm PCB).

One binary STL, three bodies, each printable without supports:
  shell  — bezel down on the bed, board drops in from the back
  cover  — flange down, skirt and snaps up
  foot   — base down; the closed shell sits in it at 18 degrees

USB-C is on the left edge, vertically centered (landscape, USB to the left).
The slot is taller than the plug so a revision a few millimetres off still fits.
"""
import os
import numpy as np
import manifold3d as m
from PIL import Image, ImageDraw
from stl import mesh as stl_mesh

m.set_circular_segments(64)

PCB_W, PCB_H = 123.0, 74.0
PLAY = 0.55
WALL = 3.2
BEZEL = 2.7
LCD_T = 4.0
REAR = 8.8
RABBET_D = 2.7
RABBET = 1.85
COVER_T = 2.4
SKIRT_T = 1.35
FIT = 0.32
TILT = 18.0
LIFT = 22.0

POCKET_W = PCB_W + 2 * PLAY
POCKET_H = PCB_H + 2 * PLAY
SHELL_W = POCKET_W + 2 * WALL
SHELL_H = POCKET_H + 2 * WALL
SHELL_D = BEZEL + LCD_T + REAR
MOUTH_W = POCKET_W + 2 * RABBET
MOUTH_H = POCKET_H + 2 * RABBET

WIN_W, WIN_H = 96.6, 55.2
USB_LEN = 22.0
USB_Z0 = BEZEL + 1.2

CRADLE_GAP = 0.50


def rr(w, h, r):
    r = min(r, w / 2 - 0.1, h / 2 - 0.1)
    core = m.CrossSection.square((w - 2 * r, h - 2 * r), center=True)
    return core.offset(r, m.JoinType.Round, circular_segments=48)


def extrude(section, h):
    return m.Manifold.extrude(section, h)


def box(x, y, z):
    return m.Manifold.cube((x, y, z), center=True)


def cyl_z(r, h):
    return m.Manifold.cylinder(h, r, circular_segments=48)


def seated(solid):
    """Bottom edge on the hinge, front toward +Y, top leaning back (-Y)."""
    return (
        solid.translate((0, SHELL_H / 2, 0))
        .rotate((90, 0, 0))
        .rotate((TILT, 0, 0))
        .translate((0, 0, LIFT))
    )


def shell():
    body = extrude(rr(SHELL_W, SHELL_H, 7.0), SHELL_D)
    # LCD module sits behind the bezel.
    lcd = box(105.5 + 0.6, 67.2 + 0.6, LCD_T + 0.6).translate(
        (0, 0, BEZEL + LCD_T / 2)
    )
    # PCB pocket, open through the back.
    pocket = box(POCKET_W, POCKET_H, REAR + RABBET_D + 1.0).translate(
        (0, 0, BEZEL + LCD_T + (REAR + RABBET_D) / 2)
    )
    # Wider mouth so the cover skirt has a rabbet outside the PCB.
    mouth = box(MOUTH_W, MOUTH_H, RABBET_D + 0.2).translate(
        (0, 0, SHELL_D - RABBET_D / 2 + 0.1)
    )
    window = box(WIN_W, WIN_H, BEZEL + 1.2).translate((0, 0, BEZEL / 2))
    # USB-C slot: open to the rear (no bridge), left side, mid-height.
    slot = box(WALL + RABBET + 6, USB_LEN, SHELL_D - USB_Z0 + 0.4).translate(
        (-SHELL_W / 2 + 0.4, 0, (USB_Z0 + SHELL_D) / 2 + 0.1)
    )
    # Fingernail notch, top, open rearward.
    notch = box(24, WALL + RABBET + 4, 7).translate(
        (0, SHELL_H / 2 - 0.6, SHELL_D - 2.6)
    )
    solid = body - lcd - pocket - mouth - window - slot - notch

    # Snap reliefs in the rabbet: a deeper lane, then a lip at the mouth.
    # Top and bottom center. The cover tab springs into the lane.
    for sign in (1, -1):
        lane = box(16, RABBET + 0.4, RABBET_D - 1.05).translate(
            (
                0,
                sign * (POCKET_H / 2 + RABBET / 2),
                SHELL_D - RABBET_D + (RABBET_D - 1.05) / 2,
            )
        )
        # Extra depth so the 0.7 mm bump can pass the lip by flexing outward.
        relief = box(14.5, 1.15, RABBET_D - 1.15).translate(
            (
                0,
                sign * (POCKET_H / 2 + RABBET - 0.35),
                SHELL_D - RABBET_D + (RABBET_D - 1.15) / 2,
            )
        )
        solid -= lane
        solid -= relief
    return solid


def cover():
    plate = extrude(rr(SHELL_W, SHELL_H, 7.0), COVER_T).translate(
        (0, 0, SHELL_D)
    )
    # Locating skirt in the rabbet. Inner opening clears the PCB.
    outer = box(MOUTH_W - 2 * FIT, MOUTH_H - 2 * FIT, RABBET_D - 0.35)
    inner = box(POCKET_W + 0.7, POCKET_H + 0.7, RABBET_D + 0.4)
    skirt = (outer - inner).translate((0, 0, SHELL_D - (RABBET_D - 0.35) / 2))
    # USB scallop so the plug housing is not pinched by the plate or skirt.
    scallop = box(18, USB_LEN + 4, COVER_T + RABBET_D + 0.4).translate(
        (-SHELL_W / 2 + 2.0, 0, SHELL_D - RABBET_D / 2 + COVER_T / 2)
    )
    vent = m.Manifold()
    for y in (12.5, 17.5, 22.5):
        vent += box(36, 1.8, COVER_T + 0.6).translate((0, y, SHELL_D + COVER_T / 2))
    bite = box(20, 3.2, COVER_T + 0.4).translate(
        (0, SHELL_H / 2 - 1.2, SHELL_D + COVER_T / 2)
    )
    solid = plate + skirt - scallop - vent - bite

    # Snap tabs: thin blade in the rabbet with a ramped bump near the lip.
    for sign in (1, -1):
        blade = box(12.5, 0.95, RABBET_D - 0.55).translate(
            (
                0,
                sign * (POCKET_H / 2 + RABBET - 0.55),
                SHELL_D - (RABBET_D - 0.55) / 2 - 0.05,
            )
        )
        # Ramp + catch bump, proud toward the shell wall.
        bump = box(11.0, 0.70, 0.85).translate(
            (
                0,
                sign * (POCKET_H / 2 + RABBET - 0.15),
                SHELL_D - RABBET_D + 1.15,
            )
        )
        solid += blade + bump
    return solid


def _features_local():
    """Lugs, heel and lip. Bottoms are buried in the plinth."""
    zc = (SHELL_D + COVER_T) / 2.0
    depth = SHELL_D + COVER_T + 1.0

    def lug(height, x):
        return box(5.4, height + 4.0, depth).translate(
            (x, -SHELL_H / 2 + height / 2 - 1.2, zc)
        )

    right = lug(17.0, SHELL_W / 2 + CRADLE_GAP + 2.7)
    left = lug(17.0, -(SHELL_W / 2 + CRADLE_GAP + 2.7))
    heel = box(96, 28, 3.2).translate(
        (0, -SHELL_H / 2 + 12.0, SHELL_D + COVER_T + CRADLE_GAP + 1.5)
    )
    lip = box(92, 8.0, 2.6).translate((0, -SHELL_H / 2 + 2.4, -CRADLE_GAP - 1.15))
    return left + right + heel + lip


def foot(guard):
    # Solid plinth: tilted deck under the shell, drafted out to a wider base.
    # Nothing floats, so the foot prints base-down without supports.
    deck = box(SHELL_W + 8, 3.2, SHELL_D + COVER_T + 4).translate(
        (0, -SHELL_H / 2 - CRADLE_GAP - 1.15, (SHELL_D + COVER_T) / 2)
    )
    deck_w = seated(deck)
    db = deck_w.bounding_box()
    base_w = (db[3] - db[0]) + 28
    base_d = (db[4] - db[1]) + 46
    base_cx = (db[0] + db[3]) / 2
    base_cy = (db[1] + db[4]) / 2 - 5
    base = extrude(rr(base_w, base_d, 32), 7.2).translate((base_cx, base_cy, 0))
    plinth = m.Manifold.batch_hull([deck_w, base])

    solid = plinth + seated(_features_local())
    front_y = solid.bounding_box()[4]
    chamfer = box(base_w + 20, 16, 16).rotate((52, 0, 0)).translate((base_cx, front_y - 1.5, 0.4))
    solid -= chamfer
    solid -= guard
    corridor = box(40, 32, 20).translate(
        (-SHELL_W / 2 - 14, 0.0, (USB_Z0 + SHELL_D) / 2)
    )
    solid -= seated(corridor)
    return solid


def usb_plug():
    """Airspace a USB-C plug needs outside the left wall."""
    plug = box(26, 14, 11).translate((-SHELL_W / 2 - 15, 0, (USB_Z0 + SHELL_D) / 2 - 1))
    return seated(plug)


def screen_slab():
    slab = box(WIN_W - 1.2, WIN_H - 1.2, 0.6).translate((0, 0, 1.6))
    return seated(slab)


def drop(solid):
    return solid.translate((0, 0, -solid.bounding_box()[2]))


def save_stl(solid, path):
    mf = solid.to_mesh()
    verts = np.asarray(mf.vert_properties, dtype=np.float64)[:, :3]
    tris = np.asarray(mf.tri_verts, dtype=np.int32)
    # Nettoyage avant écriture, en deux temps.
    # 1) Les soustractions coplanaires (chanfrein du pied, garde du berceau)
    #    laissent des triangles d'aire quasi nulle : ils suffisent à faire
    #    compter une arête non-manifold au trancheur (constaté : 1).
    v0, v1, v2 = verts[tris[:, 0]], verts[tris[:, 1]], verts[tris[:, 2]]
    area = np.linalg.norm(np.cross(v1 - v0, v2 - v0), axis=1) / 2.0
    tris = tris[area > 1e-6]
    # 2) Le STL binaire est en float32 : deux sommets confondus à 1e-9 près
    #    cessent de l'être après arrondi. On recolle à 0,1 µm puis on retire les
    #    faces rigoureusement identiques.
    uniq, inv = np.unique(np.round(verts, 4), axis=0, return_inverse=True)
    tris = inv.reshape(-1)[tris]
    _, first = np.unique(np.sort(tris, axis=1), axis=0, return_index=True)
    tris = tris[np.sort(first)]
    verts = uniq
    data = np.zeros(len(tris), dtype=stl_mesh.Mesh.dtype)
    data["vectors"] = verts[tris]
    out = stl_mesh.Mesh(data)
    out.save(path)
    bb = solid.bounding_box()
    size = (bb[3] - bb[0], bb[4] - bb[1], bb[5] - bb[2])
    print(
        f"{path}  tris={len(tris)}  verts={len(verts)}  vol={solid.volume():.0f}  "
        f"size={size[0]:.1f}x{size[1]:.1f}x{size[2]:.1f}"
    )
    return out


def mesh_arrays(solid):
    mf = solid.to_mesh()
    verts = np.asarray(mf.vert_properties, dtype=np.float64)[:, :3]
    tris = np.asarray(mf.tri_verts, dtype=np.int32)
    return verts, tris


def bed(solid):
    bb = solid.bounding_box()
    return solid.translate((-bb[0], -bb[1], -bb[2]))


def render(parts, path, eye, up=(0, 0, 1), size=(980, 740), ground=True):
    """Orthographic z-buffer. `eye` points from the object toward the camera."""
    eye = np.asarray(eye, float)
    eye = eye / np.linalg.norm(eye)
    up = np.asarray(up, float)
    right = np.cross(eye, up)
    right = right / np.linalg.norm(right)
    upv = np.cross(right, eye)
    w, h = size
    tris, cols, clouds = [], [], []
    for solid, color in parts:
        v, t = mesh_arrays(solid)
        if len(t) == 0:
            continue
        tri = v[t]
        tris.append(tri)
        cols.append(np.broadcast_to(np.asarray(color, np.float32), (len(tri), 3)).copy())
        clouds.append(tri.reshape(-1, 3))
    tri = np.concatenate(tris)
    col = np.concatenate(cols)
    pts = np.concatenate(clouds)
    center = (pts.min(0) + pts.max(0)) / 2
    rel = pts - center
    span = max(np.ptp(rel @ right), np.ptp(rel @ upv))
    scale = min(w, h) * 0.84 / max(span, 1e-6)
    d = tri - center
    p = np.stack(
        [
            w / 2 + np.tensordot(d, right, axes=([2], [0])) * scale,
            h / 2 - np.tensordot(d, upv, axes=([2], [0])) * scale + 8,
            np.tensordot(d, eye, axes=([2], [0])),
        ],
        axis=-1,
    )
    nrm = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    ln = np.linalg.norm(nrm, axis=1)
    keep = ln > 1e-8
    nrm = nrm[keep] / ln[keep, None]
    p = p[keep]
    col = col[keep]
    front = (nrm @ eye) > 0.02
    nrm, p, col = nrm[front], p[front], col[front]
    light = np.array([0.28, 0.38, 0.88])
    light /= np.linalg.norm(light)
    fill = np.array([-0.55, -0.2, 0.4])
    fill /= np.linalg.norm(fill)
    shade = 0.34 + 0.54 * np.clip(nrm @ light, 0, 1) + 0.16 * np.clip(nrm @ fill, 0, 1)
    col = np.clip(col * shade[:, None], 0, 255).astype(np.uint8)
    zb = np.full((h, w), -1e18)
    cb = np.empty((h, w, 3), np.uint8)
    cb[:] = (234, 230, 222)
    yy, xx = np.mgrid[0:h, 0:w]
    for i in range(len(p)):
        xy = p[i, :, :2]
        z = p[i, :, 2]
        minx = max(int(xy[:, 0].min()) - 1, 0)
        maxx = min(int(xy[:, 0].max()) + 1, w - 1)
        miny = max(int(xy[:, 1].min()) - 1, 0)
        maxy = min(int(xy[:, 1].max()) + 1, h - 1)
        if minx >= maxx or miny >= maxy:
            continue
        X = xx[miny : maxy + 1, minx : maxx + 1].astype(np.float64)
        Y = yy[miny : maxy + 1, minx : maxx + 1].astype(np.float64)
        x0, y0 = xy[0]
        x1, y1 = xy[1]
        x2, y2 = xy[2]
        den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(den) < 1e-5:
            continue
        w0 = ((y1 - y2) * (X - x2) + (x2 - x1) * (Y - y2)) / den
        w1 = ((y2 - y0) * (X - x2) + (x0 - x2) * (Y - y2)) / den
        w2 = 1.0 - w0 - w1
        mask = (w0 >= -0.002) & (w1 >= -0.002) & (w2 >= -0.002)
        if not np.any(mask):
            continue
        zz = w0 * z[0] + w1 * z[1] + w2 * z[2]
        sl = zb[miny : maxy + 1, minx : maxx + 1]
        update = mask & (zz > sl)
        if not np.any(update):
            continue
        sl[update] = zz[update]
        zb[miny : maxy + 1, minx : maxx + 1] = sl
        patch = cb[miny : maxy + 1, minx : maxx + 1]
        patch[update] = col[i]
        cb[miny : maxy + 1, minx : maxx + 1] = patch
    Image.fromarray(cb, "RGB").save(path)
    print("render", path)


def qa(sh, cv, ft, plug):
    inter_sc = (sh ^ cv).volume()
    inter_sf = (seated(sh) ^ ft).volume()
    inter_cf = (seated(cv) ^ ft).volume()
    plug_hit = (plug ^ ft).volume() + (plug ^ seated(sh)).volume() + (plug ^ seated(cv)).volume()
    catch = (cv.translate((0, 0, 1.6)) ^ sh).volume()
    print(
        f"QA shell^cover={inter_sc:.1f}  footGaps shell={inter_sf:.1f} cover={inter_cf:.1f}  "
        f"plugHits={plug_hit:.1f}  snapCatch={catch:.2f}"
    )
    bb = ft.bounding_box()
    print(
        f"foot {bb[3]-bb[0]:.0f} x {bb[4]-bb[1]:.0f} x {bb[5]-bb[2]:.0f} mm   "
        f"shell {SHELL_W:.1f} x {SHELL_H:.1f} x {SHELL_D:.1f}"
    )
    ok = inter_sc < 8 and inter_sf < 8 and inter_cf < 8 and plug_hit < 5 and catch > 0.2
    if not ok:
        raise SystemExit("clearance check failed")


def main():
    sh = shell()
    cv = cover()
    guard = seated(sh + cv).minkowski_sum(box(CRADLE_GAP * 2, CRADLE_GAP * 2, CRADLE_GAP * 2))
    ft = foot(guard)
    plug = usb_plug()
    qa(sh, cv, ft, plug)

    # Print layout. Shell and cover, rotated, share a 180 mm plate.
    # The foot sits on the next row — move it to a second plate.
    sh_p = bed(sh.rotate((0, 0, 90)))
    cv_p = bed(cv.rotate((180, 0, 0)).rotate((0, 0, 90)))
    ft_p = bed(ft)
    cv_p = cv_p.translate((sh_p.bounding_box()[3] + 10, 0, 0))
    row_h = max(sh_p.bounding_box()[4], cv_p.bounding_box()[4])
    ft_p = ft_p.translate((0, row_h + 16, 0))
    both = sh_p + cv_p + ft_p
    print(
        "plate shell+cover",
        f"{cv_p.bounding_box()[3]:.0f}x{row_h:.0f}",
        "foot",
        f"{ft_p.bounding_box()[3]-ft_p.bounding_box()[0]:.0f}x{ft_p.bounding_box()[4]-ft_p.bounding_box()[1]:.0f}",
    )

    # Sorties relatives au dépôt (le script vivait dans un bac à sable /workspace).
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(here, os.pardir))
    stl_dir = os.path.join(repo, "stl")
    docs_dir = os.path.join(repo, "docs")
    build_dir = os.path.join(here, "build")
    os.makedirs(stl_dir, exist_ok=True)
    os.makedirs(docs_dir, exist_ok=True)
    os.makedirs(build_dir, exist_ok=True)
    save_stl(both, os.path.join(stl_dir, "sunton-4827-stand.stl"))
    save_stl(seated(sh) + seated(cv) + ft, os.path.join(build_dir, "stand-assembled.stl"))

    bone = (246, 242, 234)
    linen = (186, 178, 166)
    ink = (42, 50, 56)
    glass = (14, 24, 28)
    asm_parts = [
        (ft, ink),
        (seated(cv), linen),
        (seated(sh), bone),
        (screen_slab(), glass),
    ]
    views = {
        "stand-three-quarter.png": (1.05, 1.2, 0.58),
        "stand-front.png": (0.0, 1.0, 0.1),
        "stand-side.png": (1.0, 0.02, 0.14),
        "stand-usb.png": (-1.0, 0.16, 0.24),
    }
    for name, eye in views.items():
        render(asm_parts, os.path.join(docs_dir, name), eye)
    render(
        [(sh_p, bone), (cv_p, linen), (ft_p, ink)],
        os.path.join(docs_dir, "stand-print.png"),
        eye=(0.05, -0.2, 1.0),
        ground=False,
        size=(1100, 860),
    )


if __name__ == "__main__":
    main()

