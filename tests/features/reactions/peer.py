"""reactions: see test.c."""
from nio import ReactionEvent, RedactionEvent

TIMEOUT = 300
THUMBS = "\U0001F44D"
HEART = "❤️"


async def peer(ctx):
    p = await ctx.peer()
    reactions, redactions = [], []
    p.c.add_event_callback(lambda room, ev: reactions.append(ev), ReactionEvent)
    p.c.add_event_callback(lambda room, ev: redactions.append(ev), RedactionEvent)
    room = await p.create_room(invite=[ctx.target], encrypted=True, name="Reaktionen")
    ctx.check(await p.wait_member(room, ctx.target, 180 * ctx.scale), "client joined")
    await p.refresh_devices(ctx.target)
    msg = await p.send(room, "Reagier mal")

    ok = await p.wait(lambda: any(r.sender == ctx.target for r in reactions), 180 * ctx.scale)
    r = next((r for r in reactions if r.sender == ctx.target), None)
    ctx.check(ok, "got the client's reaction (decrypted)")
    if not r:
        return
    ctx.check(r.reacts_to == msg, "it is for our message")
    ctx.check(r.key == THUMBS, f"key is a thumbs up ({r.key!r})")

    await p.wait(lambda: p.got(room, ctx.target, "Und du?"), 120 * ctx.scale)
    # the event ID of "Und du?": from the timeline callback
    target = p.ids.get((room, ctx.target, "Und du?"))
    ctx.check(target is not None, "got \"Und du?\"")
    if target:
        rr = await p.c.room_send(room, "m.reaction",
                                 {"m.relates_to": {"rel_type": "m.annotation", "event_id": target, "key": HEART}},
                                 ignore_unverified_devices=True)
        ctx.check(hasattr(rr, "event_id"), "reacted with a heart")

    ok = await p.wait(lambda: any(x.redacts == r.event_id for x in redactions), 180 * ctx.scale)
    ctx.check(ok, "the client took its reaction back (redaction)")
    await p.send(room, "weg ist er")
    await p.sync(1000)
