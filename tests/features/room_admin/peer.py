"""room_admin: see test.c."""
TIMEOUT = 300


async def peer(ctx):
    p = await ctx.peer()
    room = await p.wait_invite(180 * ctx.scale)
    ctx.check(room is not None, "invited")
    if not room:
        return
    ok = await p.wait_message(room, ctx.target, "fertig eingestellt", 120 * ctx.scale)
    r = p.c.rooms.get(room)
    ok = ok and r is not None
    if ok:
        jr = await p.c.room_get_state_event(room, "m.room.join_rules")
        hv = await p.c.room_get_state_event(room, "m.room.history_visibility")
        av = await p.c.room_get_state_event(room, "m.room.avatar")
        checks = [r.name == "Neuer Name ä", r.topic == "Worum es geht", jr.content.get("join_rule") == "public",
                  hv.content.get("history_visibility") == "joined", r.encrypted,
                  str(av.content.get("url", "")).startswith("mxc://"),
                  r.power_levels.get_user_level(p.user.user_id) == 50]
        ctx.check(all(checks), f"sees name, topic, join rule, history, encryption, picture, power 50 ({checks})")
        ok = all(checks)
    if ok:
        await p.refresh_devices(ctx.target)
        await p.send(room, "einstellungen ok")
    # kicked, banned, unbanned, invited again: join
    ok = await p.wait(lambda: room in p.c.invited_rooms, 180 * ctx.scale)
    ctx.check(ok, "invited again after kick/ban/unban")
    if ok:
        await p.c.join(room)
        await p.sync(1000)
