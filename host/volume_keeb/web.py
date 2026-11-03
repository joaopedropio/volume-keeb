"""The settings page: a small HTTP server on localhost serving
web/index.html, the GIF page (web/gif.html) and the JSON API they use.

Only answers requests addressed to localhost (no DNS rebinding), and every
request that changes something must carry an X-Volume-Keeb header, which a
cross-site form or fetch can't add without a CORS preflight this server
never approves.
"""
import html
import json
import re
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from . import qff
from .engine import FontUploadError
from .protocol import COLOR_FIELDS, DECORS, FONT_SLOTS, GIF_MAX_BYTES, GIF_SLOTS, MENUS, PRESETS, SAVERS, gif_info
from .tray import icon_png
from .updates import UpdateError

WEB_DIR = Path(__file__).resolve().parent / "web"
STATIC = {"/gif": ("gif.html", "text/html; charset=utf-8"), "/gif.js": ("gif.js", "text/javascript"),
          "/pixel.js": ("pixel.js", "text/javascript"), "/kva.js": ("kva.js", "text/javascript")}
MAX_FONT_BYTES = 30 * 1024 * 1024
MAX_FETCH_BYTES = 64 * 1024 * 1024
FETCH_UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36"
META_IMAGE = re.compile(r"<meta[^>]+(?:property|name)=[\"'](?:og:image|twitter:image)[\"'][^>]*>", re.I)
META_CONTENT = re.compile(r"content=[\"']([^\"']+)", re.I)
PREVIEW_TEXT = {
    "ui": "Spotify  VOL 80  MUTE",
    "title": "Daft Punk - Harder, Better",
    "list": "Firefox - Lo-fi beats 0123",
}


def fetch_image(url, follow=True):
    """Downloads a link pasted into the GIF page (image hosts send no CORS
    headers, so the page can't). A web page (Giphy, Tenor...) is followed
    to its og:image. -> (bytes, content type)"""
    if urllib.parse.urlsplit(url).scheme not in ("http", "https"):
        raise ValueError("only http(s) links")
    req = urllib.request.Request(url, headers={"User-Agent": FETCH_UA, "Accept": "image/gif,image/*;q=0.9,*/*;q=0.5"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            data = r.read(MAX_FETCH_BYTES + 1)
            ctype = r.headers.get_content_type()
            final = r.geturl()
    except urllib.error.HTTPError as e:
        raise ValueError(f"the server answered {e.code} {e.reason}") from e
    except (urllib.error.URLError, OSError) as e:
        raise ValueError(f"couldn't download it: {getattr(e, 'reason', e)}") from e
    if len(data) > MAX_FETCH_BYTES:
        raise ValueError("the file is over 64 MB")
    if ctype == "text/html":
        if follow:
            for tag in META_IMAGE.findall(data.decode("utf-8", "replace")):
                m = META_CONTENT.search(tag)
                if m:
                    return fetch_image(urllib.parse.urljoin(final, html.unescape(m.group(1))), follow=False)
        raise ValueError("that's a web page with no image link in it; copy the image's own address")
    return data, ctype


class Handler(BaseHTTPRequestHandler):
    server_version = "volume-keeb"

    def log_message(self, fmt, *args):
        pass

    # -- plumbing --------------------------------------------------------

    def _send(self, status, body, ctype):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, status=200):
        self._send(status, json.dumps(obj).encode(), "application/json")

    def _error(self, status, message):
        self._json({"error": message}, status)

    def _body(self, limit):
        length = int(self.headers.get("Content-Length") or 0)
        if length > limit:
            raise ValueError("too big")
        return self.rfile.read(length)

    def _allowed(self, mutating):
        host = (self.headers.get("Host") or "").split(":")[0]
        if host not in ("localhost", "127.0.0.1", "[::1]"):
            self._error(403, "only reachable as localhost")
            return False
        if mutating and self.headers.get("X-Volume-Keeb") != "1":
            self._error(403, "missing X-Volume-Keeb header")
            return False
        return True

    @property
    def engine(self):
        return self.server.engine

    @property
    def config(self):
        return self.server.engine.config

    def _route(self, method):
        url = urlparse(self.path)
        if not self._allowed(method != "GET"):
            return
        m = re.fullmatch(r"/api/fonts/(\w+)(/[\w.]+)?", url.path)
        if m and m.group(1) not in FONT_SLOTS:
            return self._error(404, "no such font slot")
        g = re.fullmatch(r"/api/gifs/(\w+)(/data)?", url.path)
        if g and g.group(1) not in GIF_SLOTS:
            return self._error(404, "no such gif slot")
        query = {k: v[0] for k, v in parse_qs(url.query).items()}
        try:
            if method == "GET" and url.path == "/":
                return self._send(200, (WEB_DIR / "index.html").read_bytes(), "text/html; charset=utf-8")
            if method == "GET" and url.path in STATIC:
                name, ctype = STATIC[url.path]
                return self._send(200, (WEB_DIR / name).read_bytes(), ctype)
            if method == "POST" and url.path == "/api/fetch":
                data, ctype = fetch_image(query.get("url", ""))
                return self._send(200, data, ctype)
            if g and method == "GET" and g.group(2):
                return self._send(200, self.config.gif_path(g.group(1)).read_bytes(), "application/octet-stream")
            if g and method == "POST" and not g.group(2):
                return self._upload_gif(g.group(1), query)
            if g and method == "DELETE" and not g.group(2):
                return self._clear_gif(g.group(1))
            if method == "GET" and url.path == "/favicon.png":
                return self._send(200, icon_png(), "image/png")
            if method == "GET" and url.path == "/api/state":
                return self._json(self._state())
            if method == "PUT" and url.path == "/api/theme":
                return self._put_theme()
            if method == "PUT" and url.path == "/api/screen":
                return self._put_screen()
            if method == "POST" and url.path == "/api/updates/check":
                self.server.updates.check()
                return self._json(self.server.updates.state())
            if method == "PUT" and url.path == "/api/updates/auto":
                self.config.set_check_updates(json.loads(self._body(256) or b"{}").get("on", True))
                return self._json(self.server.updates.state())
            if method == "POST" and url.path == "/api/firmware/update":
                self.server.updates.update_firmware()
                return self._json({"ok": True})
            if method == "POST" and url.path == "/api/screen/preview":
                self.engine.push_screen(preview=True)
                return self._json({"ok": True})
            if m and method == "GET" and m.group(2) == "/source":
                return self._send(200, self.config.font_path(m.group(1), "src").read_bytes(), "application/octet-stream")
            if m and method == "GET" and m.group(2) == "/preview.png":
                return self._preview(m.group(1), None, query)
            if m and method == "POST" and m.group(2) == "/preview.png":
                return self._preview(m.group(1), self._body(MAX_FONT_BYTES), query)
            if m and method == "POST" and not m.group(2):
                return self._upload_font(m.group(1), query)
            if m and method == "DELETE" and not m.group(2):
                return self._clear_font(m.group(1))
        except FileNotFoundError:
            return self._error(404, "not found")
        except (qff.FontError, UpdateError, ValueError) as e:
            return self._error(400, str(e))
        self._error(404, "not found")

    def do_GET(self):
        self._route("GET")

    def do_PUT(self):
        self._route("PUT")

    def do_POST(self):
        self._route("POST")

    def do_DELETE(self):
        self._route("DELETE")

    # -- endpoints -------------------------------------------------------

    def _state(self):
        d = self.config.data
        fonts = {}
        for slot, meta in FONT_SLOTS.items():
            fonts[slot] = {
                "label": meta["label"],
                "height": meta["height"],
                "custom": d["fonts"].get(slot),
                "job": self.engine.jobs[slot],
            }
        # Only the gif slots the knob has (gif4-6 need firmware v0.7.0).
        gif_slots = self.engine.gif_slots()
        gifs = {slot: {"custom": d["gifs"].get(slot), "job": self.engine.jobs[slot]} for slot in gif_slots}
        return {
            "connected": self.engine.knob.connected,
            "fonts_supported": self.engine.fonts_supported,
            "store_rev": self.engine.store_rev,
            "gifs_supported": self.engine.gifs_supported,
            "theme": {"preset": d["preset"], "decor": d["decor"], "colors": d["colors"]},
            "screen": d["screen"],
            "updates": self.server.updates.state(),
            "savers": [s for s in SAVERS if s not in GIF_SLOTS or s in gif_slots],
            "menus": MENUS,
            "presets": PRESETS,
            "decors": DECORS,
            "color_fields": COLOR_FIELDS,
            "fonts": fonts,
            "gifs": gifs,
            "gif_max_bytes": GIF_MAX_BYTES,
            "gif_slots": gif_slots,
            "formats": {k: v["label"] for k, v in qff.FORMATS.items()},
            "preview_text": PREVIEW_TEXT,
        }

    def _put_theme(self):
        body = json.loads(self._body(4096) or b"{}")
        self.config.set_theme(body.get("colors"), body.get("decor"), body.get("preset", ""))
        self.engine.push_theme()
        self._json({"ok": True})

    def _put_screen(self):
        self.config.set_screen(json.loads(self._body(4096) or b"{}"))
        self.engine.push_screen()
        self._json({"ok": True})

    def _preview(self, slot, font_bytes, query):
        fmt = query.get("format")
        if font_bytes is None:
            info = self.config.data["fonts"].get(slot)
            if not info:
                return self._error(404, "this slot uses the built-in font")
            if info.get("from_knob"):  # no original file to render from
                png = qff.preview_qff(self.config.font_path(slot, "qff").read_bytes(), query.get("text") or PREVIEW_TEXT[slot])
                return self._send(200, png, "image/png")
            font_bytes = self.config.font_path(slot, "src").read_bytes()
            fmt = fmt or info["format"]
        png = qff.preview(font_bytes, slot, fmt or "mono16", query.get("text") or PREVIEW_TEXT[slot])
        self._send(200, png, "image/png")

    def _upload_font(self, slot, query):
        font_bytes = self._body(MAX_FONT_BYTES)
        fmt = query.get("format", "mono16")
        data, size = qff.convert(font_bytes, slot, fmt)
        name = Path(query.get("name") or "font").name[:80]
        self.config.set_font(slot, font_bytes, data, {"name": name, "format": fmt, "size": size, "crc": zlib.crc32(data), "bytes": len(data)})
        try:
            self.engine.upload(slot, data)
        except FontUploadError as e:
            return self._json({"ok": False, "saved": True, "error": f"saved, but the upload failed: {e}"}, 502)
        self._json({"ok": True})

    def _clear_font(self, slot):
        self.config.clear_font(slot)
        try:
            self.engine.clear(slot)
        except FontUploadError as e:
            return self._json({"ok": False, "error": str(e)}, 502)
        self._json({"ok": True})

    def _upload_gif(self, slot, query):
        data = self._body(GIF_MAX_BYTES + 1)
        info = gif_info(data)
        info["name"] = Path(query.get("name") or "gif").name[:80]
        self.config.set_gif(slot, data, info)
        try:
            self.engine.upload(slot, data)
        except FontUploadError as e:
            return self._json({"ok": False, "saved": True, "error": f"saved, but the upload failed: {e}"}, 502)
        self._json({"ok": True})

    def _clear_gif(self, slot):
        self.config.clear_gif(slot)
        if self.config.data["screen"]["saver"] == slot:  # the knob would show the drive anyway
            self.config.set_screen({"saver": "drive"})
            self.engine.push_screen()
        try:
            self.engine.clear(slot)
        except FontUploadError as e:
            return self._json({"ok": False, "error": str(e)}, 502)
        self._json({"ok": True})


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, port, engine, updates):
        super().__init__(("127.0.0.1", port), Handler)
        self.engine = engine
        self.updates = updates

    @property
    def url(self):
        return f"http://localhost:{self.server_address[1]}/"

    def start(self):
        threading.Thread(target=self.serve_forever, name="web", daemon=True).start()
        print(f"settings page at {self.url}", file=sys.stderr)
