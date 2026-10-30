#!/usr/bin/env python3
"""Build the UI simulator, render every scenario and write PNGs (3x, plus a
contact sheet) to the given directory. Usage: run.py <outdir>"""
import pathlib, subprocess, sys
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
QMK = pathlib.Path.home() / "qmk_firmware"
KB = REPO / "qmk/waveshare_rp2040_zero_volume"
W, H, SCALE = 428, 142, 3

out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "sim_out").resolve()
out.mkdir(parents=True, exist_ok=True)
srcs = [str(QMK / "quantum/painter" / f) for f in
        ("qp.c", "qp_draw_core.c", "qp_draw_codec.c", "qp_draw_circle.c", "qp_draw_ellipse.c",
         "qp_draw_image.c", "qp_draw_text.c", "qp_stream.c", "qff.c", "qgf.c", "qp_comms.c")]
srcs += [str(QMK / "drivers/painter/generic" / f) for f in
         ("qp_surface_common.c", "qp_surface_rgb565.c", "qp_surface_mono1bpp.c", "qp_surface_rgb888.c")]
srcs += [str(QMK / "quantum/color.c"), str(QMK / "quantum/unicode/utf8.c"), str(QMK / "drivers/painter/comms/qp_comms_dummy.c")]
srcs += [str(KB / "graphics/font_ui.qff.c"), str(KB / "graphics/font_big.qff.c"), str(KB / "graphics/font_list.qff.c"), str(HERE / "sim.c")]
exe = out / "sim"
cmd = ["gcc", "-O1", "-w", "-o", str(exe),
       "-DQUANTUM_PAINTER_ENABLE", "-DQUANTUM_PAINTER_DUMMY_COMMS_ENABLE", "-DQUANTUM_PAINTER_SURFACE_ENABLE",
       "-DSURFACE_NUM_DEVICES=3",  # the simulated panel is a surface too
       "-DQUANTUM_PAINTER_NUM_IMAGES=8", "-DQUANTUM_PAINTER_NUM_FONTS=4",
       '-DQMK_KEYBOARD_H="quantum.h"',
       "-I", str(HERE / "stub"), "-I", str(KB), "-I", str(KB / "keymaps/volume"),
       "-I", str(QMK / "quantum"), "-I", str(QMK / "quantum/painter"),
       "-I", str(QMK / "drivers/painter/generic"), "-I", str(QMK / "drivers/painter/comms"),
       "-I", str(QMK / "quantum/unicode"), "-I", str(QMK / "platforms"),
       ] + srcs
subprocess.run(cmd, check=True)
subprocess.run([str(exe), str(out)], check=True)

frames = sorted(out.glob("*.rgb565"))
imgs = []
for f in frames:
    raw = f.read_bytes()
    img = Image.new("RGB", (W, H))
    px = img.load()
    for i in range(W * H):
        v = raw[2 * i] << 8 | raw[2 * i + 1]  # stored byte-swapped, as the panel wants it
        px[i % W, i // W] = ((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
    big = img.resize((W * SCALE, H * SCALE), Image.NEAREST)
    big.save(f.with_suffix(".png"))
    f.unlink()
    imgs.append((f.stem, big))
print("\n".join(str(out / (n + ".png")) for n, _ in imgs))
