"""message_tools: see test.c."""
from nio import RoomMessageEmote, RoomMessageText, RoomEncryptedFile, DownloadResponse
from nio.crypto.attachments import decrypt_attachment

TIMEOUT = 360
DATA = b"Datei zum Weiterleiten - Amiga rulez\n"


async def peer(ctx):
    p = await ctx.peer()
    src = []
    p.c.add_event_callback(lambda r, ev: src.append((r.room_id, ev)), (RoomMessageEmote, RoomMessageText, RoomEncryptedFile))
    rooms = []
    for _ in range(2):
        r = await p.wait_invite(180 * ctx.scale)
        if r:
            rooms.append(r)
    if len(rooms) < 2:
        ctx.check(False, "invited to both rooms")
        return
    ra = next(r for r in rooms if p.c.rooms[r].display_name == "Werkzeug A")
    rb = next(r for r in rooms if r != ra)
    await p.refresh_devices(ctx.target)

    def has(room, cond):
        return any(rr == room and cond(ev.source["content"]) for rr, ev in src)
    ok = await p.wait(lambda: has(ra, lambda c: c.get("msgtype") == "m.emote" and c.get("body") == "winkt")
                      and has(ra, lambda c: "<strong>Hallo</strong>" in c.get("formatted_body", "")
                              and p.user.user_id in c.get("m.mentions", {}).get("user_ids", []))
                      and p.c.rooms[ra].user_name(ctx.target) == "Amiga im Raum", 120 * ctx.scale)
    ctx.check(ok, "emote, formatted mention, room name")
    if not ok:
        return
    await p.send(ra, "gesehen")
    pin = await p.send(ra, "Bitte anpinnen")
    ok = await p.wait_message(ra, ctx.target, "pin gesetzt, weitergeleitet", 120 * ctx.scale)
    st = await p.c.room_get_state_event(ra, "m.room.pinned_events")
    ok = ok and pin in st.content.get("pinned", [])
    ctx.check(ok, f"pin in the room state ({getattr(st, 'content', st)})")
    ok2 = await p.wait(lambda: has(rb, lambda c: c.get("body") == "Bitte anpinnen" and "m.relates_to" not in c)
                       and any(rr == rb and isinstance(ev, RoomEncryptedFile) for rr, ev in src), 60)
    if ok2:
        fev = [ev for rr, ev in src if rr == rb and isinstance(ev, RoomEncryptedFile)][-1]
        d = await p.c.download(mxc=fev.url)
        f = fev.source["content"]["file"]
        ok2 = isinstance(d, DownloadResponse) and decrypt_attachment(d.body, f["key"]["k"], f["hashes"]["sha256"], f["iv"]) == DATA
    ctx.check(ok2, "both copies in B, the file decrypts")
    if ok and ok2:
        await p.send(ra, "pin und weiterleitung ok")
    v1 = await p.send(ra, "Version 1")
    for t in ("Version 2", "Version 3"):
        await p.c.room_send(ra, "m.room.message", {
            "msgtype": "m.text", "body": "* " + t, "m.new_content": {"msgtype": "m.text", "body": t},
            "m.relates_to": {"rel_type": "m.replace", "event_id": v1}}, ignore_unverified_devices=True)
    await p.send(ra, "editiert")
    await p.wait_message(ra, ctx.target, "ich ignoriere dich", 120 * ctx.scale)
    await p.send(ra, "bist du da?")
    ok = await p.wait_message(ra, ctx.target, "wieder gut", 120 * ctx.scale)
    await p.send(ra, "wieder da")
    await p.sync(1000)
