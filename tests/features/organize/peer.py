"""organize: see test.c."""
TIMEOUT = 300


async def peer(ctx):
    from nio import RoomCreateResponse
    p = await ctx.peer()
    server = p.user.user_id.split(":", 1)[1]
    s = await p.c.room_create(space=True, name="Hobbys", invite=[ctx.target])
    ctx.check(isinstance(s, RoomCreateResponse), "space created")
    garden = await p.create_room(invite=[ctx.target], encrypted=False, name="Garten")
    await p.c.room_put_state(s.room_id, "m.space.child", {"via": [server]}, state_key=garden)
    secret = await p.create_room(invite=[ctx.target], encrypted=True, name="Geheim")
    ok = await p.wait(lambda: p.joined(garden, ctx.target) and p.joined(secret, ctx.target), 180 * ctx.scale)
    ctx.check(ok, "the client joined")
    await p.refresh_devices(ctx.target)
    await p.send(garden, "Kuchenrezept von Oma")
    await p.send(secret, "Geheimer Kuchen")
    await p.sync(1000)
    await p.wait(lambda: False, 30)
