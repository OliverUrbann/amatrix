"""typing_receipts: see test.c."""
import asyncio
from nio import TypingNoticeEvent, ReceiptEvent

TIMEOUT = 300


async def peer(ctx):
    p = await ctx.peer()
    typing, receipts = [], []      # user lists; (event_id, type, user)

    def on_eph(room, ev):
        if isinstance(ev, TypingNoticeEvent):
            typing.append(list(ev.users))
        elif isinstance(ev, ReceiptEvent):
            for r in ev.receipts:
                receipts.append((r.event_id, r.receipt_type, r.user_id))
    p.c.add_ephemeral_callback(on_eph, (TypingNoticeEvent, ReceiptEvent))
    room = await p.wait_invite(180 * ctx.scale)
    ctx.check(room is not None, "invited")
    if not room:
        return
    ok = await p.wait(lambda: any(ctx.target in t for t in typing), 120 * ctx.scale)
    ctx.check(ok, "sees the client typing")
    await p.send(room, "tippen gesehen")
    await p.c.room_typing(room, True, 30000)
    ok = await p.wait_message(room, ctx.target, "du tippst", 120 * ctx.scale)
    await p.c.room_typing(room, False)
    ok = await p.wait_message(room, ctx.target, "lies mich", 120 * ctx.scale)
    ctx.check(ok, "got the message to read")
    if ok:
        eid = p.ids[(room, ctx.target, "lies mich")]
        await p.c.room_read_markers(room, eid, eid)
    eid = await p.send(room, "gelesen?")
    ok = await p.wait(lambda: any(r[0] == eid and r[2] == ctx.target and r[1] == "m.read" for r in receipts),
                      120 * ctx.scale)
    ctx.check(ok, "sees the client's public read receipt")
    await p.send(room, "receipt ok")
    peid = await p.send(room, "privat")
    ok = await p.wait_message(room, ctx.target, "fertig privat", 120 * ctx.scale)
    await p.sync(2000)
    await asyncio.sleep(2)
    await p.sync(2000)
    none = not any(r[0] == peid and r[2] == ctx.target for r in receipts)
    ctx.check(ok and none, f"no receipt visible for the privately read message ({receipts})")
    if ok and none:
        await p.send(room, "privat ok")
        await p.sync(1000)
