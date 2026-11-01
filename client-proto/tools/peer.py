#!/usr/bin/env python3
"""E2EE counterpart for testing the C client (matrix-nio + vodozemac).

Logs in as USER, creates an encrypted room (or reuses ROOM), invites TARGET,
waits until TARGET has joined, then sends MESSAGES encrypted messages and
ignores device verification (like a fresh Element with unverified devices).

usage: peer.py HOMESERVER USER PASSWORD TARGET [message ...]
"""
import asyncio, sys, os, json
from nio import AsyncClient, AsyncClientConfig, RoomCreateResponse, LoginResponse

async def main():
    hs, user, pw, target = sys.argv[1:5]
    msgs = sys.argv[5:] or ["Hallo Amiga! Grüße aus Python."]
    store = os.path.expanduser(f"~/.amatrix-peer/{user}")
    os.makedirs(store, exist_ok=True)
    c = AsyncClient(hs, user, store_path=store,
                    config=AsyncClientConfig(store_sync_tokens=True, encryption_enabled=True))
    cred = os.path.join(store, "login.json")
    if os.path.exists(cred):  # reuse the device so existing Olm sessions are used
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
    r = await c.room_create(name="amatrix test", invite=[target],
        initial_state=[{"type": "m.room.encryption", "state_key": "",
                        "content": {"algorithm": "m.megolm.v1.aes-sha2"}}])
    if not isinstance(r, RoomCreateResponse):
        sys.exit(f"room create failed: {r}")
    room_id = r.room_id
    print("room", room_id, flush=True)
    for _ in range(120):
        await c.sync(timeout=1000)
        room = c.rooms.get(room_id)
        if room and target in room.users:
            break
    else:
        sys.exit("target did not join")
    print("target joined", flush=True)
    for m in msgs:
        r = await c.room_send(room_id, "m.room.message",
                              {"msgtype": "m.text", "body": m},
                              ignore_unverified_devices=True)
        print("sent", getattr(r, "event_id", r), flush=True)
    await c.sync(timeout=0)
    await c.close()

asyncio.run(main())
