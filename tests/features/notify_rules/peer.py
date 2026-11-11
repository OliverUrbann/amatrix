"""notify_rules: see test.c."""
TIMEOUT = 300


async def peer(ctx):
    p = await ctx.peer()
    room = await p.wait_invite(180 * ctx.scale)
    ctx.check(room is not None, "invited")
    if not room:
        return
    await p.wait_message(room, ctx.target, "eins", 120 * ctx.scale)
    await p.send(room, "stumm 1")
    await p.wait_message(room, ctx.target, "zwei", 120 * ctx.scale)
    await p.send(room, "nur so")
    await p.send(room, "Wer will Kuchen?")
    await p.wait_message(room, ctx.target, "fertig", 120 * ctx.scale)
