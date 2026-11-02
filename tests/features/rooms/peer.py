"""rooms: public room with alias for the client to join; joins the client's
room and answers its plain message; sees the client leave."""
TIMEOUT = 240


async def peer(ctx):
    p = await ctx.peer()
    localpart = ctx.target[1:].split(":")[0]
    pub = await p.create_room(encrypted=False, name="public test room", alias=f"pub-{localpart}", public=True)
    room = await p.wait_invite(120 * ctx.scale)
    ctx.check(room is not None, "invited to the client's room")
    if not room:
        return
    ctx.check(p.c.rooms[room].display_name == "Amatrix rooms test", f"room name ({p.c.rooms[room].display_name})")
    ctx.check(p.c.rooms[room].topic == "created by the rooms feature test", "room topic")
    ctx.check(await p.wait_message(room, ctx.target, "plain hello", 120 * ctx.scale), "got plain hello")
    await p.send(room, "plain reply")
    ctx.check(await p.wait_member(pub, ctx.target, 120 * ctx.scale), "client joined public room by alias")
    ctx.check(await p.wait_left(pub, ctx.target, 120 * ctx.scale), "client left public room")
