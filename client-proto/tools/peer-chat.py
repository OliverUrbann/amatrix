#!/usr/bin/env python3
"""Two-way E2EE test against amatrix-recv -e -g GREETING (echo bot).

Creates an encrypted room, writes one message BEFORE inviting (must stay
unreadable for the Amiga), invites TARGET, waits for the join and the
encrypted greeting, sends MESSAGES and checks that every one comes back as
"Amiga hat empfangen: <text>", decrypted on this side.  Prints PASS or FAIL.

usage: peer-chat.py HOMESERVER USER PASSWORD TARGET GREETING [message ...]
"""
import asyncio, sys, os, json, time
from nio import (AsyncClient, AsyncClientConfig, RoomCreateResponse, LoginResponse,
                 RoomMessageText, MegolmEvent)


async def main():
    hs, user, pw, target, greeting = sys.argv[1:6]
    msgs = sys.argv[6:] or ["Test", "Grüße aus Köln – äöü ß", "Jo 🐄"]
    store = os.path.expanduser(f"~/.amatrix-peer/{user}")
    os.makedirs(store, exist_ok=True)
    c = AsyncClient(hs, user, store_path=store,
                    config=AsyncClientConfig(store_sync_tokens=True, encryption_enabled=True))
    cred = os.path.join(store, "login.json")
    if os.path.exists(cred):
        d = json.load(open(cred))
        c.restore_login(d["user_id"], d["device_id"], d["access_token"])
    else:
        r = await c.login(pw, device_name="amatrix-peer")
        if not isinstance(r, LoginResponse):
            sys.exit(f"login failed: {r}")
        json.dump({"user_id": r.user_id, "device_id": r.device_id,
                   "access_token": r.access_token}, open(cred, "w"))
    await c.sync(timeout=0, full_state=True)
    if c.should_upload_keys:
        await c.keys_upload()

    got, undecryptable = [], []

    def on_text(room, ev):
        if room.room_id == room_id and ev.sender == target:
            got.append(ev.body)
            print(f"  <- {ev.sender}: {ev.body}", flush=True)

    def on_megolm(room, ev):
        if room.room_id == room_id and ev.sender == target:
            undecryptable.append(ev.event_id)
            print(f"  <- {ev.sender}: UNABLE TO DECRYPT {ev.event_id}", flush=True)

    r = await c.room_create(name="amatrix chat test", initial_state=[{
        "type": "m.room.encryption", "state_key": "",
        "content": {"algorithm": "m.megolm.v1.aes-sha2"}}])
    if not isinstance(r, RoomCreateResponse):
        sys.exit(f"room create failed: {r}")
    room_id = r.room_id
    print("room", room_id, flush=True)
    c.add_event_callback(on_text, RoomMessageText)
    c.add_event_callback(on_megolm, MegolmEvent)
    await c.sync(timeout=0)
    await c.room_send(room_id, "m.room.message", {"msgtype": "m.text", "body": "vor der Einladung"},
                      ignore_unverified_devices=True)
    await c.room_invite(room_id, target)

    async def wait_for(cond, secs):
        end = time.time() + secs
        while time.time() < end and not cond():
            await c.sync(timeout=1000)
        return cond()

    if not await wait_for(lambda: c.rooms.get(room_id) and target in c.rooms[room_id].users, 120):
        print("FAIL: target did not join"); return 1
    print("target joined", flush=True)
    if not await wait_for(lambda: greeting in got, 120):
        print(f"FAIL: greeting not received (got {got}, undecryptable {undecryptable})"); return 1
    print("greeting decrypted", flush=True)
    for m in msgs:
        await c.room_send(room_id, "m.room.message", {"msgtype": "m.text", "body": m},
                          ignore_unverified_devices=True)
        print(f"  -> {m}", flush=True)
    want = [f"Amiga hat empfangen: {m}" for m in msgs]
    ok = await wait_for(lambda: all(w in got for w in want), 180)
    await c.close()
    if ok and not undecryptable:
        print(f"PASS: greeting + {len(msgs)} echoes decrypted")
        return 0
    print(f"FAIL: got {got}, undecryptable {undecryptable}")
    return 1

sys.exit(asyncio.run(main()))
