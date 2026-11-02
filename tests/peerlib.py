"""peerlib - the counterpart for the feature tests (an E2EE Matrix client
built on matrix-nio, whose crypto is vodozemac/olm like Element's).

A feature's peer.py defines

    async def peer(ctx):
        p = await ctx.peer()                 # logged-in Peer for the peer user
        room = await p.create_room(invite=[ctx.target], encrypted=True)
        ...
        ctx.check(cond, "name")              # TAP result on the peer side

ctx.target is the user ID of the client under test (the C program).
"""
import asyncio, os, time
from nio import (AsyncClient, AsyncClientConfig, LoginResponse, RoomCreateResponse,
                 RoomMessageText, MegolmEvent, JoinResponse, RoomResolveAliasResponse)


class Peer:
    def __init__(self, ctx, user):
        self.ctx, self.user = ctx, user
        store = os.path.join(ctx.workdir, "peer-" + user.localpart)
        os.makedirs(store, exist_ok=True)
        self.c = AsyncClient(ctx.server_url, user.user_id, store_path=store,
                             config=AsyncClientConfig(encryption_enabled=True, store_sync_tokens=True))
        self.messages = []        # (room_id, sender, body)
        self.undecryptable = []   # (room_id, sender, event_id)
        self.c.add_event_callback(self._on_text, RoomMessageText)
        self.c.add_event_callback(self._on_megolm, MegolmEvent)

    def _on_text(self, room, ev):
        self.messages.append((room.room_id, ev.sender, ev.body))
        self.ctx.diag(f"peer got {ev.sender}: {ev.body}")

    def _on_megolm(self, room, ev):
        self.undecryptable.append((room.room_id, ev.sender, ev.event_id))
        self.ctx.diag(f"peer cannot decrypt {ev.event_id} from {ev.sender}")

    async def login(self):
        r = await self.c.login(self.user.password, device_name="amatrix-test-peer")
        if not isinstance(r, LoginResponse):
            raise RuntimeError(f"peer login failed: {r}")
        await self.c.sync(timeout=0, full_state=True)
        if self.c.should_upload_keys:
            await self.c.keys_upload()
        return self

    async def close(self):
        await self.c.close()

    async def sync(self, ms=1000):
        await self.c.sync(timeout=ms)

    async def wait(self, cond, secs=120):
        end = time.time() + secs
        while time.time() < end:
            if cond():
                return True
            await self.c.sync(timeout=1000)
        return cond()

    # -------------------------------------------------------------- rooms
    async def create_room(self, invite=(), encrypted=True, name="amatrix test", alias=None, public=False):
        state = [{"type": "m.room.encryption", "state_key": "",
                  "content": {"algorithm": "m.megolm.v1.aes-sha2"}}] if encrypted else []
        kw = dict(name=name, invite=list(invite), initial_state=state)
        if alias:
            kw["alias"] = alias
        if public:
            from nio.api import RoomPreset, RoomVisibility
            kw["preset"] = RoomPreset.public_chat
            kw["visibility"] = RoomVisibility.public
        r = await self.c.room_create(**kw)
        if not isinstance(r, RoomCreateResponse):
            raise RuntimeError(f"room create failed: {r}")
        await self.c.sync(timeout=0)
        return r.room_id

    async def invite(self, room, user):
        await self.c.room_invite(room, user)

    async def wait_invite(self, secs=120):
        """Waits for an invite, joins it, returns the room id."""
        if not await self.wait(lambda: len(self.c.invited_rooms) > 0, secs):
            return None
        room = next(iter(self.c.invited_rooms))
        r = await self.c.join(room)
        if not isinstance(r, JoinResponse):
            raise RuntimeError(f"join failed: {r}")
        await self.c.sync(timeout=0)
        return room

    async def wait_member(self, room, user, secs=120):
        return await self.wait(lambda: room in self.c.rooms and user in self.c.rooms[room].users, secs)

    async def wait_left(self, room, user, secs=120):
        return await self.wait(lambda: room in self.c.rooms and user not in self.c.rooms[room].users, secs)

    async def refresh_devices(self, user):
        """nio caches device lists; ask again (a new or slow device may be missing)."""
        self.c.olm.users_for_key_query.add(user)
        await self.c.keys_query()

    # ----------------------------------------------------------- messages
    async def send(self, room, text):
        r = await self.c.room_send(room, "m.room.message", {"msgtype": "m.text", "body": text},
                                   ignore_unverified_devices=True)
        ok = hasattr(r, "event_id")
        self.ctx.diag(f"peer sent {text!r}: {'ok' if ok else r}")
        return r.event_id if ok else None

    def got(self, room, sender, body):
        return (room, sender, body) in self.messages

    async def wait_message(self, room, sender, body, secs=120):
        return await self.wait(lambda: self.got(room, sender, body), secs)


class Context:
    """Passed to peer(ctx); collects the peer's TAP results."""

    def __init__(self, server_url, peer_user, target_user, workdir, log):
        self.server_url, self.peer_user, self.target = server_url, peer_user, target_user.user_id
        self.target_user = target_user
        self.workdir, self.log = workdir, log
        self.results = []      # (ok, name)
        self._peers = []

    def check(self, ok, name):
        self.results.append((bool(ok), name))
        self.log(f"{'ok' if ok else 'not ok'} - peer: {name}")
        return ok

    def diag(self, text):
        self.log(f"# {text}")

    async def peer(self, user=None):
        p = await Peer(self, user or self.peer_user).login()
        self._peers.append(p)
        return p

    async def close(self):
        for p in self._peers:
            await p.close()
