"""e2ee_send: see test.c."""
TIMEOUT = 300
MSGS = ["Hallo vom Amiga", "Umlaute: äöü ÄÖÜ ß", "Emoji: 🐄"]


async def peer(ctx):
    p = await ctx.peer()
    room = await p.wait_invite(180 * ctx.scale)
    ctx.check(room is not None, "invited to the client's encrypted room")
    if not room:
        return
    ctx.check(p.c.rooms[room].encrypted, "room is encrypted")
    ok = await p.wait(lambda: all(p.got(room, ctx.target, m) for m in MSGS), 180 * ctx.scale)
    for m in MSGS:
        ctx.check(p.got(room, ctx.target, m), f"decrypted {m!r}")
    ctx.check(not [u for u in p.undecryptable if u[1] == ctx.target], "nothing from the client undecryptable")
    if ok:
        await p.refresh_devices(ctx.target)
        await p.send(room, "alle 3 gelesen")
        await p.sync(1000)
