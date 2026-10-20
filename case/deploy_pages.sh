#!/bin/sh
# Copies the 3D view (case/viewer.html and the STLs it loads) to the GitHub
# Pages site, https://joaopedropio.github.io/volume-keeb/. This repo keeps the
# canonical page; the deployed copy is that file with a header comment saying
# so. Commit and push the site repo afterwards.
#
#   case/deploy_pages.sh [path to the joaopedropio.github.io checkout]
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
SITE=${1:-$HOME/joaopedropio.github.io}
OUT=$SITE/volume-keeb
test -d "$SITE/.git"
mkdir -p "$OUT/view"
{
    sed -n 1p "$DIR/viewer.html"
    echo '<!-- Deployed copy of case/viewer.html from github.com/joaopedropio/volume-keeb: edit it there and run case/deploy_pages.sh. -->'
    sed 1d "$DIR/viewer.html"
} > "$OUT/index.html"
cp "$DIR"/volume_case-top.stl "$DIR"/volume_case-base.stl "$OUT/"
rm -f "$OUT"/view/*.stl
cp "$DIR"/view/*.stl "$OUT/view/"
echo "deployed to $OUT"
