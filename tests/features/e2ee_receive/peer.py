"""e2ee_receive: see test.c."""
TIMEOUT = 300
MSGS = ["Test", "Grüße aus Köln – äöü ß", "Jo 🐄"]


async def peer(ctx):
    p = await ctx.peer()
    room = await p.create_room(encrypted=True, name="e2ee receive test")
    await p.send(room, "vor der Einladung")
    await p.invite(room, ctx.target)
    joined = await p.wait_member(room, ctx.target, 180 * ctx.scale)
    ctx.check(joined, "client joined")
    if not joined:
        return
    await p.refresh_devices(ctx.target)
    for m in MSGS:
        ctx.check(await p.send(room, m) is not None, f"sent {m!r}")
    ok = await p.wait_message(room, ctx.target, "restart ok", 180 * ctx.scale)
    ctx.check(ok, "decrypted 'restart ok' from the client")
    ctx.check(not [u for u in p.undecryptable if u[1] == ctx.target], "nothing from the client undecryptable")
    await p.refresh_devices(ctx.target)
    await p.send(room, "nach Neustart")
    await p.sync(1000)
