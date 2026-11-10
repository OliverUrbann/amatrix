"""history_cache: see test.c."""
TIMEOUT = 300


async def peer(ctx):
    p = await ctx.peer()
    room = await p.wait_invite(180 * ctx.scale)
    ctx.check(room is not None, "invited")
    if not room:
        return
    await p.refresh_devices(ctx.target)
    await p.send(room, "eins")
    zwei = await p.send(room, "zwei")
    drei = await p.send(room, "drei")
    await p.c.room_send(room, "m.room.message", {
        "msgtype": "m.text", "body": "* zwei (neu)",
        "m.new_content": {"msgtype": "m.text", "body": "zwei (neu)"},
        "m.relates_to": {"rel_type": "m.replace", "event_id": zwei}}, ignore_unverified_devices=True)
    await p.c.room_redact(room, drei)
    await p.send(room, "fertig")
    await p.sync(2000)
