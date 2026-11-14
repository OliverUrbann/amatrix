"""replies: see test.c."""
TIMEOUT = 300


def rel(ev):
    return (ev.source.get("content") or {}).get("m.relates_to") or {}


async def peer(ctx):
    p = await ctx.peer()
    room = await p.create_room(invite=[ctx.target], encrypted=True, name="Antworten")
    ctx.check(await p.wait_member(room, ctx.target, 180 * ctx.scale), "client joined")
    await p.refresh_devices(ctx.target)
    orig = await p.send(room, "Original")

    # the client's reply
    ok = await p.wait(lambda: p.got(room, ctx.target, "Antwort"), 180 * ctx.scale)
    ctx.check(ok, "got the client's reply")
    reply = next((e for e in p.events if getattr(e, "body", None) == "Antwort"), None)
    ctx.check(reply is not None and rel(reply).get("m.in_reply_to", {}).get("event_id") == orig,
              "it points to our message (m.in_reply_to)")
    reply_id = p.ids.get((room, ctx.target, "Antwort"))

    # our reply to it, with the old quoted fallback
    await p.c.room_send(room, "m.room.message", {
        "msgtype": "m.text", "body": f"> <{ctx.target}> Antwort\n\nZurück",
        "m.relates_to": {"m.in_reply_to": {"event_id": reply_id}}}, ignore_unverified_devices=True)

    # the client edits its reply
    ok = await p.wait(lambda: any(rel(e).get("rel_type") == "m.replace" for e in p.events), 180 * ctx.scale)
    ed = next((e for e in p.events if rel(e).get("rel_type") == "m.replace"), None)
    ctx.check(ok and rel(ed).get("event_id") == reply_id
              and ed.source["content"]["m.new_content"]["body"] == "Antwort korrigiert", "got the client's edit")

    # we edit our first message, write one and delete it, write in a thread
    await p.c.room_send(room, "m.room.message", {
        "msgtype": "m.text", "body": "* Original bearbeitet",
        "m.new_content": {"msgtype": "m.text", "body": "Original bearbeitet"},
        "m.relates_to": {"rel_type": "m.replace", "event_id": orig}}, ignore_unverified_devices=True)
    gone = await p.send(room, "gleich weg")
    await p.c.room_redact(room, gone)
    await p.c.room_send(room, "m.room.message", {
        "msgtype": "m.text", "body": "im Thread",
        "m.relates_to": {"rel_type": "m.thread", "event_id": orig, "is_falling_back": True,
                         "m.in_reply_to": {"event_id": orig}}}, ignore_unverified_devices=True)

    await p.c.room_send(room, "m.room.message", {
        "msgtype": "m.text", "body": "**fett**", "format": "org.matrix.custom.html",
        "formatted_body": "<strong>fett</strong>", "m.mentions": {"user_ids": [ctx.target]}},
        ignore_unverified_devices=True)
    # a reply whose relation is only in the clear part of the encrypted event
    # (as some clients send it): inner content without m.relates_to
    import uuid
    from nio.api import Api
    from nio.responses import RoomSendResponse
    enc = p.c.olm.group_encrypt(room, {"type": "m.room.message",
                                       "content": {"msgtype": "m.text", "body": "aussen"}})
    enc["m.relates_to"] = {"m.in_reply_to": {"event_id": orig}}
    method, path, data = Api.room_send(p.c.access_token, room, "m.room.encrypted", enc, str(uuid.uuid4()))
    await p.c._send(RoomSendResponse, method, path, data, (room,))
    await p.send(room, "fertig")
    # the client writes in the thread
    ok = await p.wait(lambda: p.got(room, ctx.target, "Thread-Antwort"), 180 * ctx.scale)
    t = next((e for e in p.events if getattr(e, "body", None) == "Thread-Antwort"), None)
    ctx.check(ok and t is not None and rel(t).get("rel_type") == "m.thread" and rel(t).get("event_id") == orig,
              "the client's message is in the thread")
