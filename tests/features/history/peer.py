"""history: see test.c."""
TIMEOUT = 300


async def peer(ctx):
    p = await ctx.peer()
    room = await p.create_room(invite=[ctx.target], encrypted=True, name="history test")
    joined = await p.wait_member(room, ctx.target, 180 * ctx.scale)
    ctx.check(joined, "client joined")
    if not joined:
        return
    await p.refresh_devices(ctx.target)
    for i in range(1, 26):
        await p.send(room, f"msg {i:02d}")
    await p.sync(1000)
