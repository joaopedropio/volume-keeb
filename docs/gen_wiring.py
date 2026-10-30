#!/usr/bin/env python3
"""Writes the README's wiring picture, docs/wiring.svg: the NV3007 module, the
RP2040-Zero and the HW-040, drawn to scale (numbers from case/volume_case.scad)
and each seen from the side its wires are soldered on: the screen from the
back, the RP2040-Zero from the back (as it lies in the case, upside down) and
the encoder from the top, turned 180°. Seen that way the board's two columns
face the screen's pads and the encoder's pins in nearly the same order: the
encoder's wires go straight across, and so do the screen's first two. The
screen's VCC and GND go under the board and join the encoder's + and GND
just before their pads (one 3V3 pad, one GND pad).
Adapted from dongle-slayer's tools/gen_wiring.py. Kept in git; stdlib only.

    docs/gen_wiring.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'wiring.svg')

K = 7.0          # px per mm
P = 2.54         # pad pitch, mm
M = 5.0          # margin, mm
CAP = 6.0        # room for the captions above the parts

# The screen module, mm, from the back: its pads on the right end, BLK on top.
SW, SH = 78.0, 25.9
SPAD_X = SW - 1.65
SPAD_Y0 = 4.06
SCREEN_PINS = ['BLK', 'CS', 'DC', 'RST', 'SDA', 'SCL', 'VCC', 'GND']
# The RP2040-Zero, mm, from the back, USB-C up: the left column is the
# front's right one.
BW, BH = 18.0, 23.5
PAD0 = 1.5       # first castellation's centre from the USB end
BOARD_LEFT = ['0', '1', '2', '3', '4', '5', '6', '7', '8']
BOARD_RIGHT = ['5V', 'GND', '3V3', '29', '28', '27', '26', '15', '14']
BOARD_BOTTOM = ['9', '10', '11', '12', '13']
# The HW-040, mm, from the top, turned 180°: its pins on the left end, GND on top.
EW, EH = 26.3, 18.6
EPAD_X = EW - 24.1
EPAD_Y0 = EH - 13.06
ENC_PINS = ['GND', '+', 'SW', 'DT', 'CLK']
G1, G2 = 24.0, 20.0  # screen to board, board to encoder

# wire colour per pin; the encoder's + and GND join the screen's VCC and GND
WIRES = {'GND': '#8f8f8f', 'VCC': '#e05050', 'SCL': '#f0c040', 'SDA': '#50c0f0',
         'RST': '#c080f0', 'DC': '#60d070', 'CS': '#f09040', 'BLK': '#f4f4f4',
         '+': '#e05050', 'SW': '#c89060', 'DT': '#f070b0', 'CLK': '#5080f0'}
SCREEN_TO = {'BLK': '0', 'CS': '1', 'DC': '4', 'RST': '5', 'SDA': '3', 'SCL': '2', 'VCC': '3V3', 'GND': 'GND'}
ENC_TO = {'GND': 'GND', '+': '3V3', 'SW': '29', 'DT': '28', 'CLK': '27'}

o = []
def f(v): return f'{v * K:.1f}'
def add(s): o.append(s)
def text(x, y, size, fill, anchor, s, bold=True):
    """Words centred on y."""
    weight = ' font-weight="bold"' if bold else ''
    add(f'<text x="{f(x)}" y="{f(y + size * 0.36)}" font-size="{f(size)}"{weight} fill="{fill}" text-anchor="{anchor}">{s}</text>')

sx, sy = M, M + CAP                      # the screen's top left
by = sy + SPAD_Y0 - PAD0                 # the board's GP0 lines up with BLK
bx = sx + SW + G1
ey = by + PAD0 + P - EPAD_Y0             # the encoder's GND lines up with the board's
ex = bx + BW + G2
low = max(sy + SH, by + BH) + 2.5        # the power wires' runs under the board
W, H = ex + EW + M, low + 3.0 + M

def screen_pad(name): return sx + SPAD_X, sy + SPAD_Y0 + SCREEN_PINS.index(name) * P
def board_pad(name):
    if name in BOARD_LEFT: return bx, by + PAD0 + BOARD_LEFT.index(name) * P
    return bx + BW, by + PAD0 + BOARD_RIGHT.index(name) * P
def enc_pad(name): return ex + EPAD_X, ey + EPAD_Y0 + ENC_PINS.index(name) * P

# ---- captions
for x, w, s in ((sx, SW, 'NV3007 screen, back'), (bx, BW, 'RP2040-Zero, back'), (ex, EW, 'HW-040, top')):
    text(x + w / 2, M + 1.6, 1.7, '#e8eef2', 'middle', s)
text(bx + BW / 2, M + 3.9, 1.25, '#9aa4ad', 'middle', '(as it lies in the case)', bold=False)

# ---- the screen module, back
add(f'<rect x="{f(sx)}" y="{f(sy)}" width="{f(SW)}" height="{f(SH)}" rx="{f(0.6)}" fill="#1f70bf" stroke="#14518d" stroke-width="2"/>')
for hy in (2.95, SH - 2.95):  # mounting holes
    add(f'<circle cx="{f(sx + SW - 76.09)}" cy="{f(sy + hy)}" r="{f(1.7)}" fill="#f2f2f2"/><circle cx="{f(sx + SW - 76.09)}" cy="{f(sy + hy)}" r="{f(1.1)}" fill="#1d2126"/>')
add(f'<rect x="{f(sx + SW - 7.2)}" y="{f(sy + 6.5)}" width="{f(1.4)}" height="{f(SH - 13)}" fill="#c8962c"/>')  # the flex, folded round from the front
add(f'<rect x="{f(sx + SW - 13.5)}" y="{f(sy + 6.0)}" width="{f(6.0)}" height="{f(SH - 12)}" rx="{f(0.3)}" fill="#e9e2cf" stroke="#9a937f" stroke-width="1"/>')  # its connector
add(f'<rect x="{f(sx + 34)}" y="{f(sy + 9.5)}" width="{f(7)}" height="{f(7)}" fill="#151515"/>')  # the parts on the back
for px, py, w, h in ((28, 7, 2.0, 1.2), (28, 10, 2.0, 1.2), (28, 13, 2.0, 1.2), (44, 8, 1.2, 2.0), (46.5, 8, 1.2, 2.0),
                     (44, 15, 3.2, 1.6), (49, 12, 2.0, 1.2), (52, 17, 3.0, 2.4)):
    add(f'<rect x="{f(sx + px)}" y="{f(sy + py)}" width="{f(w)}" height="{f(h)}" fill="#2a2a2a" stroke="#c0c4c8" stroke-width="1"/>')
text(sx + 16, sy + SH / 2 - 1.2, 1.5, '#d6e6f5', 'middle', '2.79" NV3007', bold=False)
text(sx + 16, sy + SH / 2 + 1.2, 1.5, '#d6e6f5', 'middle', '142 x 428', bold=False)
for i, name in enumerate(SCREEN_PINS):
    x, y = screen_pad(name)
    if name == 'GND':  # pad 1
        add(f'<rect x="{f(x - 0.85)}" y="{f(y - 0.85)}" width="{f(1.7)}" height="{f(1.7)}" fill="#dfe3e6"/>')
    else:
        add(f'<circle cx="{f(x)}" cy="{f(y)}" r="{f(0.9)}" fill="#dfe3e6"/>')
    add(f'<circle cx="{f(x)}" cy="{f(y)}" r="{f(0.45)}" fill="#333"/>')
    text(x - 1.5, y, 1.35, '#ffffff', 'end', name)
x, y = screen_pad('GND')
text(x, y + 2.1, 1.3, '#ffffff', 'middle', '1', bold=False)

# ---- the RP2040-Zero, back
add(f'<rect x="{f(bx + BW / 2 - 4.47)}" y="{f(by - 1.0)}" width="{f(8.94)}" height="{f(4.0)}" rx="{f(1.3)}" fill="#c9ccd0" stroke="#8a8e93" stroke-width="1.5"/>')  # USB-C
add(f'<rect x="{f(bx)}" y="{f(by)}" width="{f(BW)}" height="{f(BH)}" rx="{f(0.5)}" fill="#1546a0" stroke="#0c2d6b" stroke-width="2"/>')
for i in range(9):
    y = by + PAD0 + i * P
    for edge, names, side in ((bx, BOARD_LEFT, 1), (bx + BW, BOARD_RIGHT, -1)):
        x0 = edge if side == 1 else edge - 1.9
        add(f'<rect x="{f(x0)}" y="{f(y - 0.9)}" width="{f(1.9)}" height="{f(1.8)}" rx="{f(0.5)}" fill="#d8b46a"/>')
        add(f'<circle cx="{f(edge)}" cy="{f(y)}" r="{f(0.6)}" fill="#1d2126"/>')
        text(edge + side * 2.5, y, 1.3, '#e8eef2', 'start' if side == 1 else 'end', names[i])
for i, name in enumerate(BOARD_BOTTOM):
    x = bx + BW / 2 + (i - 2) * P
    add(f'<rect x="{f(x - 0.9)}" y="{f(by + BH - 1.2)}" width="{f(1.8)}" height="{f(1.2)}" rx="{f(0.4)}" fill="#d8b46a"/>')
    add(f'<circle cx="{f(x)}" cy="{f(by + BH)}" r="{f(0.6)}" fill="#1d2126"/>')
add(f'<rect x="{f(bx + BW / 2 - 3.5)}" y="{f(by + 7.5)}" width="{f(7)}" height="{f(7)}" fill="#151515"/>')  # the RP2040
text(bx + BW / 2, by + 11.0, 1.2, '#c0c4c8', 'middle', 'RP2040', bold=False)
add(f'<rect x="{f(bx + BW / 2 - 2.5)}" y="{f(by + 16.0)}" width="{f(5)}" height="{f(3)}" fill="#151515"/>')  # the flash

# ---- the HW-040, top, turned 180°
add(f'<rect x="{f(ex)}" y="{f(ey)}" width="{f(EW)}" height="{f(EH)}" rx="{f(0.5)}" fill="#1c5fb4" stroke="#123f7a" stroke-width="2"/>')
for hx in (EW - 5.5, EW - 20.3):  # mounting holes
    add(f'<circle cx="{f(ex + hx)}" cy="{f(ey + 2.55)}" r="{f(1.75)}" fill="#d8b46a"/><circle cx="{f(ex + hx)}" cy="{f(ey + 2.55)}" r="{f(1.35)}" fill="#1d2126"/>')
text(ex + EW - 12.9, ey + 2.55, 1.3, '#e8eef2', 'middle', 'HW-040')
cx, cy = ex + EW - 10.2, ey + 10.6       # the shaft
add(f'<rect x="{f(cx - 6.0)}" y="{f(cy - 6.5)}" width="{f(12.0)}" height="{f(13.0)}" rx="{f(0.4)}" fill="#b9bec4" stroke="#6f757b" stroke-width="1.5"/>')
add(f'<circle cx="{f(cx)}" cy="{f(cy)}" r="{f(3.5)}" fill="#d6dade" stroke="#8a8e93" stroke-width="1.5"/>')
add(f'<circle cx="{f(cx)}" cy="{f(cy)}" r="{f(3.0)}" fill="#9aa0a6" stroke="#6f757b" stroke-width="1"/>')
add(f'<rect x="{f(cx - 0.7)}" y="{f(cy - 3.0)}" width="{f(1.4)}" height="{f(6.0)}" fill="#7d8389"/>')
for i, name in enumerate(ENC_PINS):
    x, y = enc_pad(name)
    add(f'<circle cx="{f(x)}" cy="{f(y)}" r="{f(0.9)}" fill="#d8b46a"/><circle cx="{f(x)}" cy="{f(y)}" r="{f(0.45)}" fill="#333"/>')
    text(x + 1.5, y, 1.35, '#ffffff', 'start', name)

# ---- the wires, a solder blob at each end
def wire(d, name):
    add(f'<path d="{d}" fill="none" stroke="#000" stroke-opacity=".55" stroke-width="{f(1.05)}" stroke-linecap="round" stroke-linejoin="round"/>')
    add(f'<path d="{d}" fill="none" stroke="{WIRES[name]}" stroke-width="{f(0.7)}" stroke-linecap="round" stroke-linejoin="round"/>')
def blob(x, y, r=0.55):
    add(f'<circle cx="{f(x)}" cy="{f(y)}" r="{f(r)}" fill="#c0c4c8" stroke="#555" stroke-width="1"/>')
def curve(a, b):
    """From a to b, leaving and arriving level."""
    mx = (a[0] + b[0]) / 2
    return f'M{f(a[0])} {f(a[1])} C{f(mx)} {f(a[1])} {f(mx)} {f(b[1])} {f(b[0])} {f(b[1])}'
def route(pts, rad=1.5):
    """Through pts, with rounded corners."""
    d = f'M{f(pts[0][0])} {f(pts[0][1])}'
    for p0, p1, p2 in zip(pts, pts[1:], pts[2:]):
        def towards(p, q, t):
            dx, dy = q[0] - p[0], q[1] - p[1]
            n = (dx * dx + dy * dy) ** 0.5
            return p[0] + dx / n * t, p[1] + dy / n * t
        a, b = towards(p1, p0, rad), towards(p1, p2, rad)
        d += f' L{f(a[0])} {f(a[1])} Q{f(p1[0])} {f(p1[1])} {f(b[0])} {f(b[1])}'
    return d + f' L{f(pts[-1][0])} {f(pts[-1][1])}'

ends = []
for name in ENC_PINS:  # straight across
    a, b = enc_pad(name), board_pad(ENC_TO[name])
    assert abs(a[1] - b[1]) < 1e-6, name  # the rows line up
    wire(f'M{f(a[0])} {f(a[1])} L{f(b[0] + 0.9)} {f(b[1])}', name)
    ends += [a, (b[0] + 0.9, b[1])]
for name in SCREEN_PINS[:6]:
    a, b = screen_pad(name), board_pad(SCREEN_TO[name])
    wire(curve(a, (b[0] - 0.9, b[1])), name)
    ends += [a, (b[0] - 0.9, b[1])]
# VCC and GND: down beside the board, under it, up the other side to the
# encoder's 3V3 and GND wires, joined there
for name, lane_l, under, lane_r in (('VCC', 4.0, low, 3.5), ('GND', 7.0, low + 2.0, 7.0)):
    a = screen_pad(name)
    j = (bx + BW + lane_r, board_pad(SCREEN_TO[name])[1])
    wire(route([a, (bx - lane_l, a[1]), (bx - lane_l, under), (j[0], under), j]), name)
    ends.append(a)
    blob(*j, r=0.75)
for e in ends:
    blob(*e)

svg = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {f(W)} {f(H)}" width="{f(W)}" height="{f(H)}" font-family="Helvetica, Arial, sans-serif">',
       f'<rect width="{f(W)}" height="{f(H)}" rx="6" fill="#1d2126"/>', *o, '</svg>']
open(OUT, 'w').write('\n'.join(svg) + '\n')
print('wrote', os.path.relpath(OUT), f'({os.path.getsize(OUT)} bytes)')
