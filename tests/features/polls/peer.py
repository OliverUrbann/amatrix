"""polls: see test.c."""
TIMEOUT = 300


async def peer(ctx):
    p = await ctx.peer()
    got = {}

    def on_unknown(room, ev):
        src = ev.source if hasattr(ev, "source") else {}
        got.setdefault(src.get("type"), []).append(src)
    from nio import UnknownEvent
    p.c.add_event_callback(on_unknown, UnknownEvent)
    room = await p.wait_invite(180 * ctx.scale)
    ctx.check(room is not None, "invited")
    if not room:
        return
    await p.refresh_devices(ctx.target)
    await p.c.room_send(room, "m.room.message", {"msgtype": "m.location", "body": "Brandenburger Tor",
                                                 "geo_uri": "geo:52.51627,13.37769"}, ignore_unverified_devices=True)
    r = await p.c.room_send(room, "org.matrix.msc3381.poll.start", {
        "org.matrix.msc3381.poll.start": {
            "question": {"org.matrix.msc1767.text": "Pizza oder Pasta?"},
            "kind": "org.matrix.msc3381.poll.disclosed", "max_selections": 1,
            "answers": [{"id": "p1", "org.matrix.msc1767.text": "Pizza"},
                        {"id": "p2", "org.matrix.msc1767.text": "Pasta"}]},
        "org.matrix.msc1767.text": "Pizza oder Pasta?\n1. Pizza\n2. Pasta"}, ignore_unverified_devices=True)
    poll = r.event_id
    ok = await p.wait(lambda: any(e.get("content", {}).get("m.relates_to", {}).get("event_id") == poll
                                  for e in got.get("org.matrix.msc3381.poll.response", [])), 120 * ctx.scale)
    ctx.check(ok, "the client's vote arrived")
    if ok:
        v = [e for e in got["org.matrix.msc3381.poll.response"]][-1]["content"]
        ctx.check(v.get("org.matrix.msc3381.poll.response", {}).get("answers") == ["a2"], f"it chose a2 ({v})")
    ref = {"rel_type": "m.reference", "event_id": poll}
    await p.c.room_send(room, "org.matrix.msc3381.poll.response",
                        {"m.relates_to": ref, "org.matrix.msc3381.poll.response": {"answers": ["p1"]}},
                        ignore_unverified_devices=True)
    await p.c.room_send(room, "org.matrix.msc3381.poll.end",
                        {"m.relates_to": ref, "org.matrix.msc3381.poll.end": {},
                         "org.matrix.msc1767.text": "Ende"}, ignore_unverified_devices=True)
    ok = await p.wait(lambda: any(e.get("content", {}).get("org.matrix.msc3381.poll.start", {})
                                  .get("question", {}).get("org.matrix.msc1767.text") == "Welcher Amiga?"
                                  for e in got.get("org.matrix.msc3381.poll.start", [])), 120 * ctx.scale)
    if ok:
        st = [e for e in got["org.matrix.msc3381.poll.start"]
              if e["content"]["org.matrix.msc3381.poll.start"]["question"]["org.matrix.msc1767.text"] == "Welcher Amiga?"][0]
        ans = [x["org.matrix.msc1767.text"] for x in st["content"]["org.matrix.msc3381.poll.start"]["answers"]]
        ok = ans == ["Amiga 500", "Amiga 1200", "Vampire"]
    ctx.check(ok, "the client's poll with its answers")
    if ok:
        await p.send(room, "umfrage ok")
        await p.sync(1000)
