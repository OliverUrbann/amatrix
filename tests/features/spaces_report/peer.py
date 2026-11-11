"""spaces_report: see test.c."""
TIMEOUT = 300


async def peer(ctx):
    from nio import RoomCreateResponse, RoomVisibility, RoomPreset
    p = await ctx.peer()
    r = await p.c.room_create(visibility=RoomVisibility.public, alias="offen-test", name="Offener Raum",
                              preset=RoomPreset.public_chat,
                              initial_state=[{"type": "m.room.history_visibility", "state_key": "",
                                              "content": {"history_visibility": "world_readable"}}])
    ctx.check(isinstance(r, RoomCreateResponse), f"public room created ({r})")
    if not isinstance(r, RoomCreateResponse):
        return
    await p.sync(1000)
    await p.send(r.room_id, "Hallo Welt")
    note = await p.create_room(invite=[ctx.target], encrypted=False, name="Hinweis")
    ok = await p.wait_message(r.room_id, ctx.target, "gemeldet", 200 * ctx.scale)
    ctx.check(ok, "the client joined and reported")
