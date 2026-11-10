"""profile: see test.c."""
from nio import ProfileGetResponse

TIMEOUT = 300


async def peer(ctx):
    p = await ctx.peer()
    room = await p.wait_invite(180 * ctx.scale)
    ctx.check(room is not None, "invited")
    if not room:
        return
    ok = await p.wait_message(room, ctx.target, "profil gesetzt", 120 * ctx.scale)
    r = await p.c.get_profile(ctx.target)
    good = ok and isinstance(r, ProfileGetResponse) and r.displayname == "Amiga-Fan ä" \
        and str(r.avatar_url).startswith("mxc://")
    ctx.check(good, f"profile of the client: {getattr(r, 'displayname', r)!r}, {getattr(r, 'avatar_url', '')!r}")
    ok2 = await p.wait(lambda: p.c.rooms[room].user_name(ctx.target) == "Amiga-Fan ä", 60)
    ctx.check(ok2, "the room shows the new name")
    if good and ok2:
        await p.send(room, "profil ok")
        await p.sync(1000)
