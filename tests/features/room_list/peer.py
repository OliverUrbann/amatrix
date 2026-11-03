"""room_list: see test.c."""
TIMEOUT = 300


async def peer(ctx):
    p = await ctx.peer()
    await p.c.set_displayname("Peer Tester")
    group = await p.create_room(invite=[ctx.target], encrypted=True, name="Amiga Freunde")
    await p.c.room_put_state(group, "m.room.topic", {"topic": "Testgruppe"})
    r = await p.c.room_create(is_direct=True, invite=[ctx.target], initial_state=[
        {"type": "m.room.encryption", "state_key": "", "content": {"algorithm": "m.megolm.v1.aes-sha2"}}])
    dm = r.room_id
    await p.sync(0)
    ok = await p.wait_member(group, ctx.target, 180 * ctx.scale) and \
         await p.wait_member(dm, ctx.target, 180 * ctx.scale)
    ctx.check(ok, "client joined group and direct chat")
    if not ok:
        return
    await p.refresh_devices(ctx.target)
    await p.send(group, "hallo gruppe 1")
    await p.send(group, "hallo gruppe 2")
    await p.send(dm, "hallo direkt")
    # the client's own direct chat
    room = await p.wait_invite(180 * ctx.scale)
    ctx.check(room not in (None, group, dm), "invited to the client's own direct chat")
    ctx.check(await p.wait_left(group, ctx.target, 180 * ctx.scale), "client left the group")
