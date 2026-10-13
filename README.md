# Sunton ESP32-4827S043 Desk Monitor Enclosure

3D-printed desk stand + wall-mount enclosure for the **Sunton ESP32-4827S043** 4.3" touchscreen board.

- **Two use cases** : desk monitor (with wedge stand) or wall-mounted display.
- **Side USB-C cutout** : vertically centered on the left side by default.
- **Parametric** : OpenSCAD source included, easy to adapt if your board revision has different USB/button placement.

## Dimensions used

Based on the manufacturer datasheet for the 4.3" ESP32-4827S043N/R/C module :

| Part | Value |
|------|-------|
| PCB | 123.0 × 74.0 × 1.6 mm |
| LCD module | 105.5 × 67.2 mm |
| Active display area | 95.04 × 53.86 mm |
| Mounting holes | 4 × Ø3.2 mm, spaced 105.5 × 67.2 mm |

> **Always compare with your actual board before printing.** Revisions vary (USB on the left/right, top/bottom, or centered).

## Files

```
stl/
  front-case.stl    # bezel + PCB pocket + USB cutout
  back-cover.stl    # flat cover with wall-mount keyholes + screw posts
  desk-stand.stl    # wedge that screws to the back cover for desk use

openscad/
  sunton-4827s043-desk-monitor.scad   # parametric source

docs/
  preview.png       # render of the three parts
```

## Print settings (Bambu P1S / generic FDM)

- **Material** : PETG or ABS/ASA for heat resistance near the ESP32.
- **Layer height** : 0.2 mm
- **Walls** : 3 perimeters
- **Infill** : 20 % gyroid
- **Supports** :
  - `front-case.stl` : none (USB slot bridges a short gap, print with the open face up).
  - `back-cover.stl` : none if printed flat (keyholes + screw posts are vertical).
  - `desk-stand.stl` : none (print with the largest flat face on the bed).

## Assembly

1. Slide the ESP32-4827S043 into the front case, screen facing out.
2. Align the four PCB mounting holes with the front case bosses.
3. Place the back cover and fasten with **4 × M2.5 self-tapping screws** (≈ 10 mm long).
4. For desk use : screw the wedge stand to the back cover using the same holes.
5. For wall use : remove the wedge and use the keyhole slots with M4 or #6 screws.

## Customizing the USB position

Open `openscad/sunton-4827s043-desk-monitor.scad` and edit :

```openscad
usb_side = "left";    // "left" or "right"
usb_y    = 37.0;      // center height from PCB top edge (mm)
usb_w    = 13.0;      // cutout width
usb_h    = 6.0;       // cutout height
```

Then re-render the STL files (OpenSCAD or `docker run openscad/openscad:latest`).

## License

MIT — Silexperience 2026
