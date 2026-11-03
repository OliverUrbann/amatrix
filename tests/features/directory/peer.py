"""directory: see test.c."""
TIMEOUT = 240


async def peer(ctx):
    p = await ctx.peer()
    lp = ctx.target_user.localpart
    first = await p.create_room(encrypted=False, name=f"Amiga Chat {lp}", alias=f"amigachat-{lp}", public=True)
    await p.c.room_put_state(first, "m.room.topic", {"topic": "Alles über den Amiga"})
    await p.create_room(encrypted=False, name=f"Retro Corner {lp}", public=True)
    await p.create_room(encrypted=False, name=f"Kaffeeklatsch {lp}", public=True)
    ctx.check(await p.wait_member(first, ctx.target, 180 * ctx.scale), "client joined from the directory")
