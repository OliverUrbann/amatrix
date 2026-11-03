"""avatars: see test.c."""
import io
from nio import UploadResponse, RoomCreateResponse

TIMEOUT = 300


def png(w, h, paint):
    from PIL import Image
    im = Image.new("RGB", (w, h))
    for y in range(h):
        for x in range(w):
            im.putpixel((x, y), paint(x, y))
    b = io.BytesIO()
    im.save(b, "PNG")
    return b.getvalue()


def jpeg(w, h, rgb):
    from PIL import Image
    b = io.BytesIO()
    Image.new("RGB", (w, h), rgb).save(b, "JPEG", quality=95)
    return b.getvalue()


async def upload(p, data, ctype, name):
    r, _ = await p.c.upload(lambda *a: io.BytesIO(data), content_type=ctype, filename=name,
                            filesize=len(data))
    if not isinstance(r, UploadResponse):
        raise RuntimeError(f"upload failed: {r}")
    return r.content_uri


async def peer(ctx):
    p = await ctx.peer()
    # room picture 120x60: green border, red centre square -> after cropping all red
    room_png = png(120, 60, lambda x, y: (220, 20, 20) if 30 <= x < 90 else (20, 200, 20))
    room_mxc = await upload(p, room_png, "image/png", "room.png")
    me_mxc = await upload(p, jpeg(64, 64, (20, 40, 230)), "image/jpeg", "me.jpg")
    await p.c.set_avatar(me_mxc)
    ctx.diag(f"room picture {room_mxc}, profile picture {me_mxc}")

    room = await p.create_room(invite=[ctx.target], encrypted=False, name="Bilderraum")
    await p.c.room_put_state(room, "m.room.avatar", {"url": room_mxc})
    r = await p.c.room_create(is_direct=True, invite=[ctx.target])
    ctx.check(isinstance(r, RoomCreateResponse), "direct chat created")
    ok = await p.wait(lambda: p.joined(room, ctx.target) and p.joined(r.room_id, ctx.target),
                      180 * ctx.scale)
    ctx.check(ok, "client joined both rooms")
