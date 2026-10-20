// volume-keeb case: a box after the MaxMix (maxmix_base_*.stl, reference
// only), widened for the 2.79" NV3007 bar (78 x 25.9 module) on a 60° face at
// the front, the 48 x 22 metal knob on the flat top behind it, sunk knob_sink
// into a plain hole (no floor, so the top lifts straight off over it), and the
// RP2040-Zero's USB-C centred in the back wall. Two prints: the top (open
// bottom down) and the base (flat).
//
//   case/gen_case.sh
//   writes case/volume_case-{top,base}.stl and the pieces of the 3D view
//   (case/view/, open index.html there). part="view" shows both with the
//   electronics in place; "check" is what the parts and the electronics share,
//   "insert" what the top meets as it comes down over the base with
//   everything on it (both should come out empty).
//
// Everything is held by the base, so the top only covers it:
// - the screen stands on two ribs behind its ends, as in dongle-slayer's case
//   (github.com/joaopedropio/dongle-slayer, case/dongle_case.scad): the right
//   one takes its two holes (M2 x 5 pan-head screws from the front of the
//   board into heat-set inserts), the left one props the other end;
// - the encoder module sits on a frame of four walls under its edges, with
//   pillars under its holes (M2 screws from above, heads up, into inserts),
//   high enough for the knob to sit knob_sink into the top while resting just
//   above the encoder's bushing;
// - the RP2040-Zero lies upside down (buttons and USB-C toward the floor) on
//   its own USB receptacle and a block under its far end, held by rails and a
//   clip; BOOT and RESET are pressed with a pin through two holes in the
//   bottom, marked B and R.
// Then the top comes down over it all and takes four M2 x 6 flat-head screws
// from below, into inserts in its bosses.
//
// No headers on anything: the wires are soldered straight to the pads.

part = "view";

// "hw040" is the only module measured so far; "ky040" is a placeholder for
// when its numbers are in (see enc_dims below).
encoder = "hw040";

// ---- the screen module, mm, seen from the front as it sits; from dongle-
// slayer's case (its maker's drawing, GoldenMorning, 8 pins; glass_t measured)
mod_w = 78.0;
mod_h = 25.9;
pcb_t = 1.6;
glass_t = 2.5;          // glass + backlight, in front of the PCB
glass_x = [4.06, 73.49]; // the backlight, from the module's left edge
glass_h = 24.38;        // centred
area_x = [5.46, 69.60]; // the lit area
area_h = 21.28;         // centred
hole_x = 76.09;         // the two mounting holes, from the left edge
hole_y = 2.95;          // from the top and the bottom edges (20 apart)
pads_x = 1.65;          // the 8 pads the wires go to (no header)
pads_span = [4.06, 21.84];
back_room = 2.5;        // parts and the flex's connector on the module's back,
back_x = [10, 52];      // only here: the ends are bare

// ---- the encoder module, mm: u along the PCB from the end away from the pads,
// v across it from the edge with the two holes, heights from the PCB's
// underside. HW-040 from hw-040-encoder-module.stp and
// hw-040-module-dimensions.webp (in the repo root, untracked).
//   [pcb [l, w, t], holes [[u, v], ...], hole_d, shaft [u, v],
//    body [l, w, top], bushing [d, top], shaft [d, top],
//    frame walls [u = 0, v = 0, u = l, v = w], frame notches [[[u0, v0], [u1, v1]], ...]]
// The frame's walls stay clear of what comes through the board underneath:
// the encoder's three pins (u 2.6-4.1), the switch's two (u 16.4-17.7), the
// pads (u ~24.1, so the wall at u = l is thinner) and the body's lugs (u
// 8.7-11.7 at v ~4.2 and ~17.0: the one by the far edge gets a notch).
function enc_dims(name) =
    name == "hw040" ? [[26.3, 18.6, 1.55], [[5.5, 2.55], [20.3, 2.55]], 3.5, [10.2, 10.6],
                       [12.0, 13.0, 6.84], [7.0, 13.0], [6.0, 22.0],
                       [1.6, 1.6, 1.2, 1.4], [[[8.2, 16.4], [12.2, 18.7]]]]
  : undef;  // "ky040": not measured yet
E = enc_dims(encoder);
assert(E != undef, str("encoder \"", encoder, "\": no measurements yet (only hw040 has them)"));
enc_pcb = E[0];
enc_holes = E[1];
enc_hole_d = E[2];
enc_shaft = E[3];
enc_body = E[4];
enc_bush = E[5];
enc_shaft_top = E[6];
enc_walls = E[7];
enc_notches = E[8];

// ---- the knob
knob_d = 48.0;
knob_h = 22.0;
knob_bore = 15.0;       // how deep the shaft can go into it
knob_sink = 7.0;        // how much of it is inside the top
knob_gap = 0.5;         // between its underside and the bushing
knob_clr = 1.0;         // around it, in the hole
skirt_t = 1.6;          // the hole's wall, knob_sink deep

// ---- the RP2040-Zero, from its STEP model (rp2040-zero-2.snapshot.6/, without
// headers): seen from its top face (buttons, USB-C), x from its middle, y from
// the USB end
zero_w = 18.0;
zero_l = 23.5;
zero_t = 1.1;
zusb_w = 8.94;
zusb_dn = 3.26;         // the receptacle past the top face (it's mid-mount)
zusb_up = 1.0;          // and into the board
zusb_len = 7.32;
zusb_out = 1.0;         // past the board's edge
zbtn = [3.2, 4.6, 2.5]; // BOOT and RESET
zbtn_x = 4.63;          // BOOT at -x, RESET at +x
zbtn_y = 16.12;         // from the USB end
zchip_t = 0.9;          // the RP2040 on the other face

// ---- the box
wall = 2.0;
r = 2.0;                // edge rounding
angle = 60;
foot = 3.0;             // the front edge's height
border_b = 4.0;         // face around the module, along the slope
border_t = 3.0;
side = 4.0;             // wall + room beside the module
depth = 84.0;
clr = 0.4;              // module in the face
gap = 0.2;              // between the glass and the face
fit = 0.25;             // base in the top
base_t = 2.0;
btn_hole_d = 2.8;       // pin holes under BOOT and RESET
// Every screw goes into a heat-set insert: M2, 3 long, 3.5 across (the common
// one), pressed into a 3.2 hole with room for the screw's tip past it.
insert_d = 3.2;
insert_l = 4.5;
tip_d = 2.2;
tip_l = 2.5;

$fn = 48;

// the lit area sits off the module's middle: more room on the left evens the
// face around it
skew = (mod_w - area_x[1]) - area_x[0];
W = mod_w + 2 * side + skew;
L = border_b + mod_h + clr + border_t;      // the face, along the slope
run = L * cos(angle);
ztop = foot + L * sin(angle);

// the face: u along X, v up the slope, w out of it (w < 0 is inside)
module face() {
    multmatrix([[1, 0, 0, 0],
                [0, cos(angle), -sin(angle), 0],
                [0, sin(angle), cos(angle), foot],
                [0, 0, 0, 1]]) children();
}
mod_u = side + skew;         // the module's left edge on the face
mod_v = border_b + clr / 2;  // its bottom edge
w_glass = -wall - gap;       // the glass's front
w_pcb = w_glass - glass_t;   // the board's front
w_back = w_pcb - pcb_t;      // and its back
// a point of the face's frame (v, w) in the side view (y, z)
function at(v, w) = [v * cos(angle) - w * sin(angle), foot + v * sin(angle) + w * cos(angle)];

module profile(inset = 0) {
    offset(delta = -inset) polygon([[0, -8], [0, foot], [run, ztop], [depth, ztop], [depth, -8]]);
}
module along_x(x0, len) { translate([x0, 0, 0]) rotate([90, 0, 90]) linear_extrude(len) children(); }

// the knob: centred across, a little ahead of the flat top's middle so the
// RP2040 fits behind the encoder; it rests knob_gap above the bushing
knob_x = W / 2;
knob_y = 49.0;
knob_z = ztop - knob_sink;                 // its underside
enc_z = knob_z - knob_gap - enc_bush[1];   // the encoder PCB's underside
assert(enc_shaft_top[1] - enc_bush[1] - knob_gap <= knob_bore, "the shaft is longer than the knob's bore");
hole_r = knob_d / 2 + knob_clr;
// the encoder PCB's corner (u = 0, v = 0) in the box: pads toward +x, holes
// toward the front
function enc_at(p) = [knob_x - enc_shaft[0] + p[0], knob_y - enc_shaft[1] + p[1]];

// the RP2040-Zero: centred, its USB end just inside the back wall
zero_y1 = depth - wall - 0.2;
zero_y0 = zero_y1 - zero_l;
zero_face = base_t + zusb_dn;              // its top face, now underneath
zusb_z = zero_face - zusb_dn + (zusb_dn + zusb_up) / 2;
zusb_h = zusb_dn + zusb_up;
btn_xy = [[W / 2 - zbtn_x, zero_y1 - zbtn_y], [W / 2 + zbtn_x, zero_y1 - zbtn_y]];  // BOOT, RESET

module top() {
    difference() {
        union() {
            difference() {
                intersection() {
                    minkowski() {
                        along_x(r, W - 2 * r) offset(delta = -r) profile();
                        sphere(r, $fn = 24);
                    }
                    translate([-1, -1, 0]) cube([W + 2, depth + 2, ztop + 5]);
                }
                along_x(wall, W - 2 * wall) profile(wall);
            }
            bosses();
            translate([knob_x, knob_y, knob_z]) cylinder(r = hole_r + skirt_t, h = ztop - knob_z - 0.01);
        }
        window();
        translate([knob_x, knob_y, knob_z - 1]) cylinder(r = hole_r, h = ztop, $fn = 120);
        translate([knob_x, knob_y, ztop - 0.6]) cylinder(r1 = hole_r, r2 = hole_r + 1, h = 0.61, $fn = 120);  // chamfered
        usb_port();
        for (b = screw_xy) translate([b[0], b[1], base_t - 0.01]) insert_hole();
    }
}

module window() {
    m = 0.5;
    x0 = mod_u + area_x[0] - m; x1 = mod_u + area_x[1] + m;
    y0 = mod_v + (mod_h - area_h) / 2 - m; y1 = y0 + area_h + 2 * m;
    face() hull() {
        translate([x0, y0, -wall - 1]) cube([x1 - x0, y1 - y0, 1]);
        translate([x0, y0, -0.5]) cube([x1 - x0, y1 - y0, 0.5]);
        translate([x0 - 1.6, y0 - 1.6, 0]) cube([x1 - x0 + 3.2, y1 - y0 + 3.2, 1]);  // chamfered
    }
}

// M2 screws from below, into inserts in bosses joined to the side walls (the
// inserts go in from below, the top upside down). The front pair can't be in
// the corners, where the screen's lower edge comes down nearly to the base:
// they sit along the sides, behind the screen's ribs.
front_y = 33.0;
screw_xy = [[wall + 4, depth - wall - 4], [W - wall - 4, depth - wall - 4],
            [wall + 4, front_y], [W - wall - 4, front_y]];
module bosses() {
    for (i = [0:3]) let(x = screw_xy[i][0], y = screw_xy[i][1], back = i < 2)
        hull() {
            translate([x, y, base_t]) cylinder(d = 7, h = 7);
            translate([x < W / 2 ? wall - 0.5 : x, back ? y : y - 3.5, base_t]) cube([4.5, back ? 4.5 : 7, 7]);
        }
}

// an insert's hole, from its mouth up (+z)
module insert_hole() {
    cylinder(d = insert_d, h = insert_l + 0.01);
    cylinder(d = tip_d, h = insert_l + tip_l);
}

// The receptacle stops short of the wall's face (the board can't go into the
// wall), so the outside is sunk around it for the plug's overmold to seat. It
// comes up from below with the base, through a notch the base fills.
plug_w = 12.4;
plug_h = 7.0;
plug_sink = 1.0;
module usb_port() {
    translate([W / 2, depth + 1, zusb_z]) rotate([90, 0, 0]) rounded(zusb_w + 0.6, zusb_h + 0.6, 1.4, 5);
    translate([W / 2, depth + 1, zusb_z]) rotate([90, 0, 0]) rounded(plug_w, plug_h, 2.5, 1 + plug_sink);
    translate([W / 2 - (zusb_w + 0.6) / 2, depth - wall - 1, -1]) cube([zusb_w + 0.6, wall + 2, zusb_z + 1]);
}
usb_floor = zusb_z - (zusb_h + 0.6) / 2;  // the port's opening, from below
module rounded(w, h, rr, len) {
    hull() for (sx = [-1, 1], sy = [-1, 1])
        translate([sx * (w / 2 - rr), sy * (h / 2 - rr), 0]) cylinder(r = rr, h = len);
}

module base() {
    x0 = wall + fit; y0 = wall + fit;
    difference() {
        union() {
            translate([x0, y0, 0]) cube([W - 2 * x0, depth - wall - fit - y0, base_t]);
            ribs();
            enc_mount();
            zero_holder();
            // fills the notch under the USB port, flush with the back
            translate([W / 2 - zusb_w / 2 - 0.15, depth - wall - fit - 0.01, 0]) {
                cube([zusb_w + 0.3, wall + fit + 0.01, min(base_t, usb_floor)]);
                translate([0, fit - 0.05, 0]) cube([zusb_w + 0.3, wall + 0.06, usb_floor]);
            }
        }
        translate([W / 2, depth + 1, zusb_z]) rotate([90, 0, 0]) rounded(plug_w, plug_h, 2.5, 1 + plug_sink);
        for (b = screw_xy) translate([b[0], b[1], -1]) {
            cylinder(d = 2.4, h = 5);
            cylinder(d1 = 4.6, d2 = 2.4, h = 1.1 + 1);  // countersunk M2
        }
        // BOOT and RESET, pressed with a pin from below; the letters read from
        // below with the box tipped over its front edge
        for (i = [0:1]) translate([btn_xy[i][0], btn_xy[i][1], 0]) {
            translate([0, 0, -1]) cylinder(d = btn_hole_d, h = base_t + 2);
            translate([0, -btn_hole_d / 2 - 0.8, -0.01]) mirror([0, 1, 0])
                linear_extrude(0.6) text(i == 0 ? "B" : "R", size = 3, halign = "center", valign = "bottom");
        }
    }
}

// Everything behind the screen's back between v0 and v1, down to the base and
// back to y1: a rib from u0 to u1. Its front drops straight to the base, so it
// prints without overhangs.
module rib(u0, u1, v0, v1, y1) {
    down = (base_t - foot - v1 * sin(angle)) / cos(angle);  // w where v1 meets the base
    intersection() {
        along_x(u0, u1 - u0) polygon([[at(v0, w_back)[0], base_t - 0.01], at(v0, w_back), at(v1, w_back), at(v1, down)]);
        translate([wall + fit, 0, base_t - 0.01]) cube([W - 2 * (wall + fit), y1, ztop]);
        along_x(0, W) profile(wall + fit);  // clear of the top's slope
    }
}

// The right one takes the screws, into inserts pressed in from the front, so
// it reaches past the module's end and edges to keep plastic around them; the
// left one only props the other end, clear of the pads.
module ribs() {
    rib(mod_u + 4.5, mod_u + 9, mod_v, mod_v + mod_h, 23);
    difference() {
        rib(mod_u + hole_x - 3.6, mod_u + mod_w + 1.6, mod_v - 1.5, mod_v + mod_h + 1.5, 28);
        face() for (y = [hole_y, mod_h - hole_y]) translate([mod_u + hole_x, mod_v + y, w_back + 0.01])
            mirror([0, 0, 1]) insert_hole();
    }
}

// A frame of four walls under the encoder PCB's edges, with the pillars under
// its holes inside it: M2 screws from above into inserts. The whole edge
// bears on it, so a click can't bend the board.
module enc_mount() {
    o = enc_at([0, 0]);
    h = enc_z - base_t + 0.01;
    t = enc_walls;
    translate([o[0], o[1], base_t - 0.01]) difference() {
        union() {
            difference() {
                cube([enc_pcb[0], enc_pcb[1], h]);
                translate([t[0], t[1], -1]) cube([enc_pcb[0] - t[0] - t[2], enc_pcb[1] - t[1] - t[3], h + 2]);
            }
            for (p = enc_holes) translate([p[0], p[1], 0]) cylinder(d = 5.5, h = h);
        }
        for (n = enc_notches) translate([n[0][0], n[0][1], h - 2]) cube([n[1][0] - n[0][0], n[1][1] - n[0][1], 3]);  // the lug's tip
        for (p = enc_holes) translate([p[0], p[1], h + 0.01]) mirror([0, 0, 1]) insert_hole();
    }
}

// The board lies on its own receptacle, between two cheeks that keep it
// straight, and on a block under its far end (clear of the pads there); rails
// at its far corners and a clip over its far edge hold it down. The clip
// backs onto the encoder's frame, so it doesn't give: the board goes in
// tilted, far edge under the hook first, then the USB end down between the
// cheeks. The USB port in the wall holds that end once the top is on. The
// corners and the far edge are pins the knob doesn't use (GP8, GP9-GP15).
module zero_holder() {
    cx = W / 2;
    for (s = [-1, 1]) translate([cx + s * (zusb_w / 2 + 0.2) + (s < 0 ? -1.5 : 0), zero_y1 - zusb_len + zusb_out + 0.5, base_t - 0.01])
        cube([1.5, zusb_len - zusb_out - 0.8, zusb_dn - 0.3 + 0.01]);
    translate([cx - 5, zero_y0 + 1.5, base_t - 0.01]) cube([10, 3, zusb_dn + 0.01]);
    for (s = [-1, 1]) translate([cx + s * (zero_w / 2 + 0.2) + (s < 0 ? -1.2 : 0), zero_y0 - 0.4, base_t - 0.01])
        cube([1.2, 4.4, zusb_dn + zero_t + 0.8]);
    translate([cx - 2.5, zero_y0 - 0.2 - 1.6, base_t - 0.01]) {
        cube([5, 1.6, zusb_dn + zero_t + 0.2 + 1.0]);
        translate([0, 0, zusb_dn + zero_t + 0.2]) hull() {
            cube([5, 1.6 + 1.0, 0.6]);
            cube([5, 1.6, 1.0]);
        }
    }
}

// ---- stand-ins for the electronics, in pieces of one color each for the 3D
// view (which = "all" or a piece's name)
module screen(which = "all") {
    any = function(n) which == "all" || which == n;
    face() translate([mod_u, mod_v, w_glass]) mirror([0, 0, 1]) {
        if (any("pcb")) color("#1f70bf") translate([0, 0, glass_t]) difference() {
            cube([mod_w, mod_h, pcb_t]);
            for (y = [hole_y, mod_h - hole_y]) translate([hole_x, y, -1]) cylinder(d = 2.2, h = 4);
        }
        if (any("glass")) color("#111") translate([glass_x[0], (mod_h - glass_h) / 2, 0]) cube([glass_x[1] - glass_x[0], glass_h, glass_t]);
        if (any("parts")) color("#333") translate([back_x[0], 2, glass_t + pcb_t]) cube([back_x[1] - back_x[0], mod_h - 4, back_room]);
        if (any("metal")) color("#ccc") for (y = [hole_y, mod_h - hole_y]) translate([hole_x, y, glass_t - 1.6]) cylinder(d = 4.0, h = 1.6);  // the screws' heads (M2 pan)
    }
}
module enc(which = "all") {
    any = function(n) which == "all" || which == n;
    o = enc_at([0, 0]);
    translate([o[0], o[1], enc_z]) {
        if (any("pcb")) color("#1f5fbf") difference() {
            cube(enc_pcb);
            for (h = enc_holes) translate([h[0], h[1], -1]) cylinder(d = enc_hole_d, h = 4);
        }
        if (any("metal")) color("#c9ccd0") {
            for (h = enc_holes) translate([h[0], h[1], enc_pcb[2]]) cylinder(d = 3.8, h = 1.3);  // the screws' heads
            translate([enc_shaft[0], enc_shaft[1], 0]) {
                translate([0, 0, enc_body[2]]) cylinder(d = enc_bush[0], h = enc_bush[1] - enc_body[2]);
                translate([0, 0, enc_bush[1]]) cylinder(d = enc_shaft_top[0], h = enc_shaft_top[1] - enc_bush[1]);
            }
        }
        if (any("body")) color("#888") translate([enc_shaft[0] - enc_body[0] / 2, enc_shaft[1] - enc_body[1] / 2, enc_pcb[2]])
            cube([enc_body[0], enc_body[1], enc_body[2] - enc_pcb[2]]);
    }
}
module knob() {
    color("#9a9ea4") translate([knob_x, knob_y, knob_z]) cylinder(d = knob_d, h = knob_h, $fn = 120);
}
module zero(which = "all") {
    any = function(n) which == "all" || which == n;
    translate([W / 2, zero_y1, zero_face]) {
        if (any("pcb")) color("#1f3fbf") translate([-zero_w / 2, -zero_l, 0]) cube([zero_w, zero_l, zero_t]);
        if (any("parts")) color("#222") translate([-3.5, -zero_l + 7.6, zero_t]) cube([7, 7, zchip_t]);  // the RP2040
        if (any("usb")) color("#c9ccd0") hull() for (x = [-1, 1])  // rounded ends, like the real one
            translate([x * (zusb_w - zusb_h) / 2, zusb_out - zusb_len, (zusb_up - zusb_dn) / 2]) rotate([-90, 0, 0]) cylinder(d = zusb_h, h = zusb_len);
        if (any("buttons")) color("#eee") for (s = [-1, 1]) translate([s * zbtn_x - zbtn[0] / 2, -zbtn_y - zbtn[1] / 2, -zbtn[2]]) cube(zbtn);
    }
}
module electronics() { screen(); enc(); knob(); zero(); }

echo(str("SIZE W ", W, " x D ", depth, " x H ", ztop, "; knob underside ", knob_z,
         ", encoder PCB at ", enc_z, " (pillars ", enc_z - base_t, ")"));

if (part == "top") top();
else if (part == "base") base();
else if (part == "check") intersection() { union() { top(); base(); } electronics(); }
else if (part == "insert") intersection() {
    top();
    for (dz = [0.3, 1, 2, 3, 4, 6, 8, 10, 13, 16, 20, 25, 30, 36]) translate([0, 0, -dz]) union() { base(); electronics(); }
}
else if (part == "screen_pcb") screen("pcb");
else if (part == "screen_glass") screen("glass");
else if (part == "screen_parts") screen("parts");
else if (part == "screen_metal") screen("metal");
else if (part == "enc_pcb") enc("pcb");
else if (part == "enc_body") enc("body");
else if (part == "enc_metal") enc("metal");
else if (part == "knob") knob();
else if (part == "zero_pcb") zero("pcb");
else if (part == "zero_parts") zero("parts");
else if (part == "zero_usb") zero("usb");
else if (part == "zero_buttons") zero("buttons");
else if (part == "view") {
    color("#3a3a3a") top();
    color("#5a5a5a") base();
    electronics();
}
