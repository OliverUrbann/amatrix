"""room_admin: see test.c."""
import asyncio
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
    # kicked, banned, unbanned; the room only takes knocks now: knock, get let in
    await asyncio.sleep(3)
    for _ in range(60):
        r = await p.c.room_knock(room) if hasattr(p.c, "room_knock") else None
        if r is None:
            from nio.api import Api
            method, path, data = "POST", Api._build_path(["knock", room], {"access_token": p.c.access_token}), "{}"
            resp = await p.c.send(method, path, data, headers={"Content-Type": "application/json"})
            okk = resp.status == 200
        else:
            okk = type(r).__name__.endswith("Response") and "Error" not in type(r).__name__
        if okk:
            break
        await asyncio.sleep(2)
    ctx.check(okk, "knocked")
    ok = await p.wait(lambda: room in p.c.invited_rooms, 180 * ctx.scale)
    ctx.check(ok, "invited again after kick/ban/unban and a knock")
    if ok:
        await p.c.join(room)
        await p.sync(1000)
