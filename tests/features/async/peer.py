"""async: see test.c."""
TIMEOUT = 360


async def peer(ctx):
    p = await ctx.peer()
    room = await p.create_room(invite=[ctx.target], encrypted=True, name="async test")
    joined = await p.wait_member(room, ctx.target, 180 * ctx.scale)
    ctx.check(joined, "client joined through the network process")
    if not joined:
        return
    await p.refresh_devices(ctx.target)
    await p.send(room, "ping 1")
    got = await p.wait_message(room, ctx.target, "pong 1", 120 * ctx.scale)
    ctx.check(got, "got 'pong 1' (sent while the client was in its long poll)")
    if got:
        await p.send(room, "ping 2")
    # the client creates a second room and invites us
    room2 = await p.wait_invite(120 * ctx.scale)
    ctx.check(room2 is not None, "invited to the room the client created")
    await p.sync(1000)
