"""sessions: see test.c (the peer is not needed)."""
TIMEOUT = 200


async def peer(ctx):
    ctx.check(True, "nothing to do")
