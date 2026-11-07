"""media: see test.c."""
import io
from PIL import Image

TIMEOUT = 300


def picture(w, h, fmt, colour=(220, 30, 30)):
    im = Image.new("RGB", (w, h), colour)
    for x in range(w // 2, w):                      # right half blue: orientation check
        for y in range(h):
            im.putpixel((x, y), (30, 30, 220))
    b = io.BytesIO()
    im.save(b, fmt)
    return b.getvalue()


async def upload(p, data, mime, name, encrypt):
    resp, keys = await p.c.upload(lambda *a: io.BytesIO(data), content_type=mime, filename=name,
                                  encrypt=encrypt, filesize=len(data))
    return resp.content_uri, keys


def efile(uri, keys):
    return {"url": uri, "key": keys["key"], "iv": keys["iv"], "hashes": keys["hashes"], "v": "v2"}


async def peer(ctx):
    p = await ctx.peer()
    room = await p.create_room(invite=[ctx.target], encrypted=True, name="Bilder")
    ctx.check(await p.wait_member(room, ctx.target, 180 * ctx.scale), "client joined")
    await p.refresh_devices(ctx.target)

    # 1: encrypted PNG 400x300, no preview of its own
    png = picture(400, 300, "PNG")
    uri, keys = await upload(p, png, "image/png", "rot-blau.png", True)
    await p.c.room_send(room, "m.room.message", {
        "msgtype": "m.image", "body": "rot-blau.png", "file": efile(uri, keys),
        "info": {"mimetype": "image/png", "size": len(png), "w": 400, "h": 300}}, ignore_unverified_devices=True)

    # 2: encrypted JPEG with an encrypted preview (like Element)
    big = picture(1200, 900, "JPEG", (30, 200, 30))
    small = picture(120, 90, "JPEG", (30, 200, 30))
    uri, keys = await upload(p, big, "image/jpeg", "gross.jpg", True)
    turi, tkeys = await upload(p, small, "image/jpeg", "thumb.jpg", True)
    await p.c.room_send(room, "m.room.message", {
        "msgtype": "m.image", "body": "gross.jpg", "file": efile(uri, keys),
        "info": {"mimetype": "image/jpeg", "size": len(big), "w": 1200, "h": 900,
                 "thumbnail_file": efile(turi, tkeys),
                 "thumbnail_info": {"mimetype": "image/jpeg", "w": 120, "h": 90}}}, ignore_unverified_devices=True)

    # 3: an encrypted file
    text = "Hallo Datei aus dem Test\n".encode()
    uri, keys = await upload(p, text, "text/plain", "hallo.txt", True)
    await p.c.room_send(room, "m.room.message", {
        "msgtype": "m.file", "body": "hallo.txt", "file": efile(uri, keys),
        "info": {"mimetype": "text/plain", "size": len(text)}}, ignore_unverified_devices=True)

    # 4: unencrypted picture (server preview)
    uri, _ = await upload(p, picture(640, 480, "PNG"), "image/png", "offen.png", False)
    await p.c.room_send(room, "m.room.message", {
        "msgtype": "m.image", "body": "offen.png", "url": uri,
        "info": {"mimetype": "image/png", "w": 640, "h": 480}}, ignore_unverified_devices=True)
    # 5: a gallery (MSC4274): two pictures in one message
    items = []
    for name, col in (("eins.png", (220, 30, 30)), ("zwei.png", (30, 200, 30))):
        d = picture(200, 150, "PNG", col)
        u, k = await upload(p, d, "image/png", name, True)
        items.append({"itemtype": "m.image", "body": name, "file": efile(u, k),
                      "info": {"mimetype": "image/png", "size": len(d), "w": 200, "h": 150}})
    await p.c.room_send(room, "m.room.message", {"msgtype": "dm.filament.gallery", "body": "Zwei Bilder",
                                                 "itemtypes": items}, ignore_unverified_devices=True)
    # 6: a sticker
    d = picture(64, 64, "PNG")
    u, k = await upload(p, d, "image/png", "sticker.png", True)
    await p.c.room_send(room, "m.sticker", {"body": "Aufkleber", "file": efile(u, k),
                                            "info": {"mimetype": "image/png", "w": 64, "h": 64}},
                        ignore_unverified_devices=True)
    await p.send(room, "fertig")
    await p.sync(1000)
