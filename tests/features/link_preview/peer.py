"""link_preview: see test.c."""
import io
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

TIMEOUT = 300
PAGE = """<!doctype html><html><head><meta charset="utf-8">
<meta property="og:title" content="Amiga-Seite">
<meta property="og:description" content="Alles über den Amiga">
<meta property="og:image" content="/bild.png">
<title>Amiga</title></head><body>Hallo</body></html>"""


def png():
    from PIL import Image
    b = io.BytesIO()
    Image.new("RGB", (40, 30), (30, 90, 200)).save(b, "PNG")
    return b.getvalue()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/seite.html"):
            body, ct = PAGE.encode(), "text/html; charset=utf-8"
        elif self.path.startswith("/bild.png"):
            body, ct = png(), "image/png"
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def serve():
    srv = HTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


async def peer(ctx):
    srv = serve()
    p = await ctx.peer()
    room = await p.wait_invite(180 * ctx.scale)
    ctx.check(room is not None, "invited")
    if not room:
        return
    await p.send(room, f"Schau mal: http://127.0.0.1:{srv.server_port}/seite.html")
    ok = await p.wait_message(room, ctx.target, "vorschau ok", 120 * ctx.scale)
    ctx.check(ok, "the client got the preview")
    srv.shutdown()
