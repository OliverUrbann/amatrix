"""send_media: see test.c."""
from nio import (RoomMessageImage, RoomMessageFile, RoomEncryptedImage, RoomEncryptedFile, DownloadResponse)
from nio.crypto.attachments import decrypt_attachment

TIMEOUT = 360
PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d49484452000000030000000208020000001216f14d00000014494441"
    "54785e633c2122c200064c108a8181010011d000f462ea4f100000000049454e44ae426082")
BIG = bytes(((i * 7 + i // 251) & 255) for i in range(200000))


async def peer(ctx):
    p = await ctx.peer()
    got = []      # (room, msgtype, body, encrypted, source)

    def on_media(room, ev):
        src = ev.source.get("content", {})
        got.append((room.room_id, src.get("msgtype"), ev.body, isinstance(ev, (RoomEncryptedImage, RoomEncryptedFile)),
                    ev, src))
        ctx.diag(f"peer got attachment {ev.body} ({src.get('msgtype')}) in {room.room_id}")

    for cls in (RoomMessageImage, RoomMessageFile, RoomEncryptedImage, RoomEncryptedFile):
        p.c.add_event_callback(on_media, cls)
    rooms = []
    for _ in range(2):
        r = await p.wait_invite(180 * ctx.scale)
        if r:
            rooms.append(r)
    ctx.check(len(rooms) == 2, "invited to both rooms")
    if len(rooms) < 2:
        return
    enc = [r for r in rooms if p.c.rooms[r].encrypted]
    ctx.check(len(enc) == 1, "one of them encrypted")
    if not enc:
        return
    enc = enc[0]
    ok = await p.wait(lambda: len(got) >= 4, 200 * ctx.scale)
    ctx.check(ok, f"four attachments arrived ({len(got)})")

    async def fetch(ev, src, encrypted):
        r = await p.c.download(mxc=ev.url)
        if not isinstance(r, DownloadResponse):
            ctx.diag(f"download failed: {r}")
            return None
        if not encrypted:
            return r.body
        f = src["file"]
        return decrypt_attachment(r.body, f["key"]["k"], f["hashes"]["sha256"], f["iv"])

    good = 0
    for room, mt, body, encrypted, ev, src in got:
        data = await fetch(ev, src, encrypted)
        info = src.get("info", {})
        if body in ("rot.png", "Bild"):
            ok1 = data == PNG and mt == "m.image" and info.get("w") == 3 and info.get("h") == 2 \
                and info.get("mimetype") == "image/png" and info.get("size") == len(PNG)
            ok1 = ok1 and encrypted == (room == enc)
            ctx.check(ok1, f"{body} in the {'encrypted' if room == enc else 'unencrypted'} room intact, m.image 3x2")
        elif body == "gross.bin":
            ok1 = data == BIG and mt == "m.file" and encrypted and info.get("size") == len(BIG)
            ctx.check(ok1, "gross.bin intact, encrypted m.file")
        else:
            ok1 = False
            ctx.check(False, f"unexpected attachment {body}")
        good += ok1
    if good == 4:
        await p.refresh_devices(ctx.target)
        await p.send(enc, "dateien ok")
        await p.sync(1000)
