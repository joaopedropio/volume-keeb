"""The tray icon (pystray): "Settings" opens the page in the browser, and
an extra item shows up when a newer release is out.

Optional: without pystray, or without a tray to put it in, the app runs
the same, minus the icon, and the page is opened by hand or with --open.
"""
import io
import sys
import webbrowser

from PIL import Image, ImageDraw


def icon_image(size=64):
    """A knob seen from above: a ring with a pointer, in the neon theme's colors."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    s = size / 64
    d.ellipse([2 * s, 2 * s, 62 * s, 62 * s], fill=(28, 10, 46, 255), outline=(255, 64, 190, 255), width=round(5 * s))
    d.ellipse([16 * s, 16 * s, 48 * s, 48 * s], fill=(16, 6, 28, 255), outline=(60, 230, 255, 255), width=round(3 * s))
    d.line([32 * s, 32 * s, 44 * s, 18 * s], fill=(60, 230, 255, 255), width=round(5 * s))
    return img


def icon_png(size=64):
    buf = io.BytesIO()
    icon_image(size).save(buf, "PNG")
    return buf.getvalue()


def run(url, autostart=None, updates=None):
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
    icon = pystray.Icon("volume-keeb", icon_image(), "volume-keeb", menu=pystray.Menu(*items))
    if updates is not None:
        updates.on_change = icon.update_menu
    try:
        icon.run()
    except Exception as e:
        print(f"no tray icon ({e}); open {url} by hand", file=sys.stderr)
        return False
    return True
