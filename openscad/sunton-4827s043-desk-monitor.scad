/*
 * Sunton ESP32-4827S043 Desk Monitor Enclosure
 * --------------------------------------------
 * Parametric three-piece enclosure :
 *  - front case (bezel + PCB pocket + USB cutout)
 *  - back cover (flat, wall-mount keyholes, screw posts)
 *  - desk stand (separate wedge, screws into back cover)
 *
 * PCB reference (from manufacturer datasheet):
 *   - PCB : 123.0 x 74.0 x 1.6 mm
 *   - LCD module : 105.5 x 67.2 mm
 *   - Active area : 95.04 x 53.86 mm
 *   - Mounting holes : 4 x D3.2, entraxe 105.5 x 67.2 mm
 *
 * User-specific constraint :
 *   - USB-C port on the side, vertically centered.
 */

// ---------- Rendering toggle ----------
part = "front";    // [front, back, stand, all]

// ---------- PCB dimensions ----------
pcb_w       = 123.0;
pcb_h       = 74.0;
pcb_t       = 1.6;

// ---------- Screen ----------
mod_w       = 105.5;      // LCD module width
mod_h       = 67.2;       // LCD module height
active_w    = 95.04;
active_h    = 53.86;

// ---------- Mounting holes ----------
hole_dx     = 105.5;
hole_dy     = 67.2;
hole_d      = 3.2;
hole_r      = hole_d / 2;

// ---------- Enclosure parameters ----------
wall        = 2.5;
tol         = 0.4;        // play around PCB
body_d      = 16.0;       // depth of front case
front_t     = 3.0;        // thickness of front bezel

usb_w       = 13.0;       // USB-C cutout width
usb_h       = 6.0;        // USB-C cutout height
usb_side    = "left";     // [left, right]
usb_y       = pcb_h / 2;  // vertically centered

screw_d     = 2.5;        // self-tapping screw
boss_d      = 7.0;
back_t      = 2.5;

// ---------- Desk stand parameters ----------
stand_w     = 90.0;
stand_d     = 45.0;
stand_h_back= 22.0;       // high side
stand_h_front= 5.0;       // low side
stand_angle = atan((stand_h_back - stand_h_front) / stand_d);

// ---------- Computed ----------
body_w      = pcb_w + 2*wall + 2*tol;
body_h      = pcb_h + 2*wall + 2*tol;
pocket_w    = pcb_w + 2*tol;
pocket_h    = pcb_h + 2*tol;
pocket_d    = body_d - front_t;

// ---------- Helpers ----------
module rounded_rect(w, h, r) {
    offset(r = r) offset(delta = -r) square([w, h], center = true);
}

module front_case() {
    difference() {
        union() {
            // Main body
            linear_extrude(height = body_d)
                rounded_rect(body_w, body_h, 4);

            // Front bezel lip
            translate([0, 0, body_d - 0.5])
                linear_extrude(height = 0.5)
                    rounded_rect(body_w + 1, body_h + 1, 4);
        }

        // PCB pocket
        translate([0, 0, front_t])
            linear_extrude(height = pocket_d + 0.02)
                rounded_rect(pocket_w, pocket_h, 2);

        // Screen opening (active area + margin)
        translate([0, 0, body_d - front_t - 0.01])
            linear_extrude(height = front_t + 0.02)
                square([active_w + 1.5, active_h + 1.5], center = true);

        // Touch module clearance
        translate([0, 0, front_t])
            linear_extrude(height = body_d - front_t + 0.01)
                square([mod_w + 0.5, mod_h + 0.5], center = true);

        // USB-C cutout (side, centered vertically)
        usb_x = (usb_side == "left") ? -body_w/2 - 0.01 : body_w/2 + 0.01;
        translate([usb_x, 0, front_t + pcb_t + usb_h/2])
            cube([wall + 0.04, usb_w, usb_h], center = true);
    }

    // Mounting bosses (front half)
    for (dx = [-hole_dx/2, hole_dx/2])
        for (dy = [-hole_dy/2, hole_dy/2])
            translate([dx, dy, front_t])
                difference() {
                    cylinder(h = pocket_d - 2, d = boss_d, $fn = 32);
                    cylinder(h = pocket_d, d = screw_d, $fn = 32);
                }
}

module back_cover() {
    total_w = body_w;
    total_h = body_h;

    difference() {
        union() {
            // Back plate
            linear_extrude(height = back_t)
                rounded_rect(total_w, total_h, 4);

            // Perimeter lip to fit front case
            translate([0, 0, back_t])
                difference() {
                    linear_extrude(height = 3)
                        rounded_rect(total_w - 0.5, total_h - 0.5, 3.5);
                    linear_extrude(height = 3.1)
                        rounded_rect(total_w - 5, total_h - 5, 2);
                }

            // Screw posts (long enough to also screw the stand)
            for (dx = [-hole_dx/2, hole_dx/2])
                for (dy = [-hole_dy/2, hole_dy/2])
                    translate([dx, dy, back_t])
                        difference() {
                            cylinder(h = body_d - front_t - back_t + 2, d = boss_d, $fn = 32);
                            cylinder(h = body_d + 4, d = screw_d, $fn = 32);
                        }
        }

        // Wall-mount keyholes (2x2 grid, on back face)
        for (x = [-30, 30])
            for (y = [-20, 20])
                translate([x, y, -0.01])
                    keyhole();
    }
}

module keyhole() {
    linear_extrude(height = back_t + 0.02) {
        union() {
            circle(d = 8, $fn = 24);
            translate([0, -12, 0])
                square([4, 14], center = true);
        }
    }
}

module desk_stand() {
    // Wedge: print with large face down => no supports
    // Back face angle = stand_angle from horizontal
    difference() {
        // Solid wedge
        rotate([90, 0, 0])
            linear_extrude(height = stand_w, center = true, convexity = 2)
                polygon(points=[
                    [-stand_d/2, 0],
                    [stand_d/2, 0],
                    [stand_d/2, stand_h_front],
                    [-stand_d/2, stand_h_back]
                ]);

        // Screw holes into back cover (use two lower holes)
        // Back cover hole spacing is hole_dx x hole_dy.
        // We place the stand holes at the same spacing, near the bottom.
        for (dx = [-hole_dx/2, hole_dx/2])
            translate([dx, -stand_w/2 + 10, 4])
                rotate([-90, 0, 0])
                    cylinder(h = stand_w - 12, d = screw_d, $fn = 32);
    }
}

// ---------- Render ----------
if (part == "front" || part == "all")
    color("cyan") front_case();

if (part == "back" || part == "all")
    translate([part == "all" ? body_w + 20 : 0, 0, 0])
        color("magenta") back_cover();

if (part == "stand" || part == "all")
    translate([part == "all" ? -body_w - 20 : 0, 0, 0])
        color("yellow") desk_stand();
