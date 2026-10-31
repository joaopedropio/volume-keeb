#!/usr/bin/env python3
"""GIF -> screensaver tool: serves the page on localhost and opens it.

The page can't download a pasted link itself (most image hosts send no CORS
headers), so it asks /fetch?url=... here instead. A link to a page (Giphy,
Tenor, ...) rather than to the image is followed to its og:image.

    python3 tools/gif_saver/serve.py [--port 8139] [--no-browser]
"""
import argparse
import html
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
FILES = {"/": "index.html", "/index.html": "index.html", "/gif.js": "gif.js", "/pixel.js": "pixel.js"}
TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8"}
MAX_BYTES = 64 << 20
UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36"
META_IMAGE = re.compile(r"<meta[^>]+(?:property|name)=[\"'](?:og:image|twitter:image)[\"'][^>]*>", re.I)
CONTENT = re.compile(r"content=[\"']([^\"']+)", re.I)


class FetchError(Exception):
    pass


def fetch(url, follow=True):
    if urllib.parse.urlsplit(url).scheme not in ("http", "https"):
        raise FetchError("only http(s) links")
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "image/gif,image/*;q=0.9,*/*;q=0.5"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            data = r.read(MAX_BYTES + 1)
            ctype = r.headers.get_content_type()
            final = r.geturl()
    except urllib.error.HTTPError as e:
        raise FetchError(f"the server answered {e.code} {e.reason}") from e
    except (urllib.error.URLError, OSError) as e:
        raise FetchError(f"couldn't download it: {getattr(e, 'reason', e)}") from e
    if len(data) > MAX_BYTES:
        raise FetchError("the file is over 64 MB")
    if ctype == "text/html":
        if follow:
            page = data.decode("utf-8", "replace")
            for tag in META_IMAGE.findall(page):
                m = CONTENT.search(tag)
                if m:
                    return fetch(urllib.parse.urljoin(final, html.unescape(m.group(1))), follow=False)
        raise FetchError("that's a web page with no image link in it; copy the image's own address")
    return data, ctype


class Handler(BaseHTTPRequestHandler):
    def send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        if u.path in FILES:
            f = HERE / FILES[u.path]
            self.send(200, f.read_bytes(), TYPES[f.suffix])
        elif u.path == "/fetch":
            url = urllib.parse.parse_qs(u.query).get("url", [""])[0]
            try:
                data, ctype = fetch(url)
            except FetchError as e:
                self.send(502, str(e).encode(), "text/plain; charset=utf-8")
            else:
                self.send(200, data, ctype)
        else:
            self.send(404, b"not found", "text/plain")

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8139)
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = f"http://localhost:{args.port}/"
    print(f"GIF saver on {url} (Ctrl+C to stop)")
    if not args.no_browser:
        webbrowser.open(url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
