"""This app's version: the repo's VERSION file, which the firmware build
reads too. The Windows build bundles it next to the code."""
import sys
from pathlib import Path


def _read():
    here = Path(__file__).resolve().parent
    for path in (Path(getattr(sys, "_MEIPASS", here)) / "VERSION", here.parent.parent / "VERSION"):
        try:
            return path.read_text().strip()
        except OSError:
            pass
    return "0.0.0"


VERSION = _read()


def parse(version):
    """"v1.2.3" or "1.2.3" -> (1, 2, 3); None if it isn't one."""
    try:
        parts = tuple(int(p) for p in version.lstrip("v").split("."))
    except (AttributeError, ValueError):
        return None
    return parts if len(parts) == 3 else None
