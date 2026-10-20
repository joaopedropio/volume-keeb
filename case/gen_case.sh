#!/bin/sh
# Exports the case (case/volume_case.scad) as the two prints,
# case/volume_case-top.stl and case/volume_case-base.stl, and the pieces of
# the 3D view (case/viewer.html), case/view/*.stl (all kept in git: run it
# again after changing the model), then runs the model's two checks: what the
# parts share with the electronics, and what the top meets on its way down over
# the base; both must come out empty but for touching faces.
#
#   case/gen_case.sh
#
# Needs OpenSCAD 2021 or later (openscad on the PATH) and python3 ($PYTHON).
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
SCAD=$DIR/volume_case.scad
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for p in top base; do
    openscad -q -o "$DIR/volume_case-$p.stl" -D "part=\"$p\"" "$SCAD"
done
# the electronics' stand-ins for case/viewer.html, a color each
mkdir -p "$DIR/view"
for p in screen_pcb screen_glass screen_parts screen_metal enc_pcb enc_body enc_metal knob \
         zero_pcb zero_parts zero_usb zero_buttons; do
    openscad -q -o "$DIR/view/$(echo "$p" | tr _ -).stl" -D "part=\"$p\"" "$SCAD"
done
# Only touching faces may be left (the ribs meet the screen's back), so what
# comes out must have no volume to speak of.
for p in check insert; do
    out=$(openscad -o "$TMP/$p.stl" -D "part=\"$p\"" "$SCAD" 2>&1 || true)  # empty is an error to it
    case $out in *"object is empty"*) echo "$p: empty"; continue ;; esac
    vol=$("${PYTHON:-python3}" -I - "$TMP/$p.stl" <<'PY'
import re, sys
v = [tuple(map(float, m)) for m in re.findall(r'vertex\s+(\S+)\s+(\S+)\s+(\S+)', open(sys.argv[1]).read())]
print(abs(sum((a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6
              for a, b, c in zip(v[0::3], v[1::3], v[2::3]))))
PY
)
    if "${PYTHON:-python3}" -c "import sys; sys.exit(float(sys.argv[1]) >= 0.01)" "$vol"; then
        echo "$p: only touching faces ($vol mm3)"
    else
        echo "$p: $vol mm3 overlap, see it with: openscad -D 'part=\"$p\"' $SCAD" >&2
        exit 1
    fi
done
