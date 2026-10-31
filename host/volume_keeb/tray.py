"""The tray icon (pystray): "Settings" opens the page in the browser, and
an extra item shows up when a newer release is out. The icon's screen is
green while the knob is connected, red while it isn't.

Optional: without pystray, or without a tray to put it in, the app runs
the same, minus the icon, and the page is opened by hand or with --open.
"""
import io
import math
import sys
import webbrowser

from PIL import Image, ImageDraw


def _lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def icon_image(size=64, connected=True):
    """The knob itself, in a slight perspective: the case's top face with the
    knob's cap coming out of its hole, and the screen on the front lit green,
    or red when the knob isn't plugged in. Drawn at 4x on a 64-unit grid,
    then trimmed to what was drawn and scaled down, for smooth edges."""
    k = 4
    img = Image.new("RGBA", (64 * k, 64 * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def box(*v):
        return [round(x * k) for x in v]

    d.polygon(box(10, 18, 54, 18, 61, 28, 3, 28), fill=(84, 90, 104))  # top face
    d.rounded_rectangle(box(3, 27, 61, 61), radius=4 * k, fill=(52, 56, 66))  # front face
    d.line(box(3, 28, 61, 28), fill=(110, 116, 132), width=k)
    d.ellipse(box(16, 18.5, 48, 27.5), fill=(24, 26, 32))  # the hole

    # The cap: a cylinder, shaded across its side.
    cx, rx, ry, top, bottom = 32, 14, 3.8, 16, 22
    for x in range(round((cx - rx) * k), round((cx + rx) * k)):
        u = ((x + 0.5) / k - cx) / rx
        yb = (bottom + ry * math.sqrt(max(0.0, 1 - u * u))) * k
        d.line([x, top * k, x, yb], fill=_lerp((210, 214, 222), (100, 104, 116), min(1.0, abs(u + 0.35) / 1.1)))
    d.ellipse(box(cx - rx, top - ry, cx + rx, top + ry), fill=(176, 180, 190), outline=(222, 226, 234), width=k)

    # The screen, with a lighter band across the top as glass.
    lit = (46, 214, 96) if connected else (236, 64, 64)
    d.rectangle(box(8, 32, 56, 57), fill=(10, 11, 14))
    d.rectangle(box(10, 34, 54, 55), fill=lit)
    d.rectangle(box(10, 34, 54, 38.5), fill=_lerp(lit, (255, 255, 255), 0.3))

    x0, y0, x1, y1 = img.getbbox()
    w, h = x1 - x0, y1 - y0
    side = max(w, h) + 2 * k
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.paste(img.crop((x0, y0, x1, y1)), ((side - w) // 2, (side - h) // 2))
    return square.resize((size, size), Image.LANCZOS)


def icon_png(size=64):
    buf = io.BytesIO()
    icon_image(size).save(buf, "PNG")
    return buf.getvalue()


def run(url, autostart=None, updates=None, knob=None):
    """Shows the icon and blocks until Quit; returns False (at once) if
    there's no tray to show it in."""
    try:
        import pystray
    except Exception as e:  # ImportError, or a backend that can't load
        print(f"no tray icon ({e}); open {url} by hand", file=sys.stderr)
        return False

    items = [pystray.MenuItem("Settings", lambda: webbrowser.open(url), default=True)]
    if updates is not None:
        items.append(pystray.MenuItem(
            lambda item: f"Update available: v{updates.latest['version']}" if updates.latest else "",
            lambda: webbrowser.open(url + "#updates"),
            visible=lambda item: updates.app_update() or updates.firmware_update(),
        ))
    if autostart is not None:
        items.append(pystray.MenuItem(
            "Start with Windows",
            lambda: autostart.set_enabled(not autostart.enabled()),
            checked=lambda item: autostart.enabled(),
        ))
    items.append(pystray.MenuItem("Quit", lambda icon: icon.stop()))
    images = {True: icon_image(), False: icon_image(connected=False)}

    def title(connected):
        return "volume-keeb" if connected else "volume-keeb (knob not connected)"

    icon = pystray.Icon("volume-keeb", images[True], title(True), menu=pystray.Menu(*items))
    if updates is not None:
        updates.on_change = icon.update_menu
    if knob is not None:
        def on_connection(connected):
            icon.icon = images[connected]
            icon.title = title(connected)
        knob.on_change = on_connection
        on_connection(knob.connected)  # in case it changed before the hook was in
    try:
        icon.run()
    except Exception as e:
        print(f"no tray icon ({e}); open {url} by hand", file=sys.stderr)
        return False
    return True
