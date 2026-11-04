"""store_crypt: see test.c."""
TIMEOUT = 300


async def peer(ctx):
    p = await ctx.peer()
    room = await p.create_room(invite=[ctx.target], encrypted=True, name="Tresor")
    joined = await p.wait_member(room, ctx.target, 180 * ctx.scale)
    ctx.check(joined, "client joined")
    if joined:
        await p.refresh_devices(ctx.target)
        await p.send(room, "vorher geheim")
        await p.sync(1000)
