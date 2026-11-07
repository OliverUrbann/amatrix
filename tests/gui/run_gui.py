#!/usr/bin/env python3
"""GUI test of the MUI client in FS-UAE, against a private Synapse.

    tests/gui/run_gui.py [-v]

The Amiga runs Amatrix (build/amiga/gui/Amatrix) on its Workbench screen and
a small AmigaDOS loop that executes command files the host drops into the
Extra: drawer (ARexx scripts for Amatrix's port, screenshot requests).  The
peer (matrix-nio) chats with it from the host.  Checks are TAP lines; the
screenshots land in build/amiga/gui-shots/.

Needs MUI in the emulator: tests/emu/setup-emu-mui.sh.
"""
import argparse, asyncio, io, os, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
TOP = os.path.dirname(TESTS)
sys.path.insert(0, os.path.join(TESTS, "server"))
sys.path.insert(0, TESTS)
from server import TestServer, ensure_venv   # noqa: E402

TESTENV = os.environ.get("TESTENV", os.path.expanduser("~/amiga/testenv/amiga-testenv"))
SHOTS = os.path.join(TOP, "build", "amiga", "gui-shots")

LOOP = """Stack 262144
Assign AmiSSL: SYS:AmiSSL
Assign LIBS: AmiSSL:Libs ADD
{verbose}
Run >NIL: Extra:Amatrix >SYS:gui-out.txt SERVER={server} USER={user} PASSWORD={password}
Lab loop
If EXISTS Extra:go
  rx Extra:c.rexx >Extra:res.txt
  Delete Extra:go QUIET
EndIf
If EXISTS Extra:stop
  Skip end
EndIf
Wait 1
Skip loop BACK
Lab end
Wait 3
Echo "done" >SYS:amiga-done
"""


class Amiga:
    """Remote control of the emulated Amiga through the Extra: drawer."""

    def __init__(self, extra, log):
        self.extra, self.log, self.n = extra, log, 0

    async def go(self, secs=60):
        """Lets the Amiga run Extra:c.rexx (output to Extra:res.txt); waits until done."""
        open(os.path.join(self.extra, "go"), "w").close()
        end = time.time() + secs
        while time.time() < end:
            if not os.path.exists(os.path.join(self.extra, "go")):
                return True
            await asyncio.sleep(0.5)
        self.log(f"# amiga: rexx script not done in {secs}s")
        return False

    async def rexx(self, *cmds, secs=60):
        """Sends ARexx commands to AMATRIX; returns RC and RESULT of the last."""
        self.n += 1
        lines = ["/* */", "options results", "address 'AMATRIX.1'"]
        for c in cmds:
            lines.append("'" + c.replace("'", "''") + "'")     # a string: sent to the host
        lines.append("say 'RC=' || rc")
        lines.append("say 'RESULT=' || result")
        with open(os.path.join(self.extra, "c.rexx"), "w", encoding="latin-1") as f:
            f.write("\n".join(lines) + "\n")
        res = os.path.join(self.extra, "res.txt")
        if os.path.exists(res):
            os.remove(res)
        await self.go(secs)
        out = open(res, encoding="latin-1").read() if os.path.exists(res) else ""
        rc, result = None, ""
        for line in out.splitlines():
            if line.startswith("RC="):
                rc = line[3:].strip()
            elif line.startswith("RESULT="):
                result = line[7:].strip()
        return rc, result

    async def rexx_raw(self, lines, secs=60):
        """Runs an ARexx script as written (e.g. 'address command ...')."""
        with open(os.path.join(self.extra, "c.rexx"), "w", encoding="latin-1") as f:
            f.write("/* */\n" + "\n".join(lines) + "\n")
        return await self.go(secs)

    async def shot(self, name):
        """Screenshot of the Amiga screen, taken by the host (works even if the Amiga hangs)."""
        await asyncio.sleep(2)
        disp = ":" + os.environ.get("AMIGA_DISPLAY", "42")
        p = await asyncio.create_subprocess_shell(
            f"xwd -display {disp} -root -silent | convert xwd:- -crop 692x540+166+114 +repage "
            f"{os.path.join(SHOTS, name + '.png')}")
        await p.wait()

    async def until(self, cond, secs):
        end = time.time() + secs
        while time.time() < end:
            v = await cond()
            if v:
                return v
            await asyncio.sleep(1)
        return await cond()


def png(w, h, rgb):
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (w, h), rgb)
    d = ImageDraw.Draw(im)
    d.ellipse((w // 4, h // 4, 3 * w // 4, 3 * h // 4), fill=(255, 255, 255))
    b = io.BytesIO()
    im.save(b, "PNG")
    return b.getvalue()


async def upload(p, data, name):
    from nio import UploadResponse
    r, _ = await p.c.upload(lambda *a: io.BytesIO(data), content_type="image/png", filename=name,
                            filesize=len(data))
    if not isinstance(r, UploadResponse):
        raise RuntimeError(f"upload failed: {r}")
    return r.content_uri


async def store_part(ctx, amiga, online, was_verified=False):
    check = ctx.check
    # passphrase for the Amatrix data: set it, restart, unlock
    rc, _ = await amiga.rexx("STOREPASS Tresor 42")
    check(rc == "0", "store passphrase command")
    import glob

    def encrypted():
        f = glob.glob(os.path.join(amiga.extra, "Amatrix-*", "login.json"))
        return f and open(f[0], encoding="latin-1").read().startswith("AMXE1 ")
    end = time.time() + 120
    while time.time() < end and not encrypted():
        await asyncio.sleep(2)
    check(encrypted(), "login.json in the drawer is encrypted")
    await amiga.rexx_raw(["address command 'Avail >Extra:avail-before.txt'"])
    await amiga.rexx("QUIT")
    for _ in range(30):                      # until the old instance is really gone
        await amiga.rexx_raw(["address command 'Status >Extra:status-now.txt'"])
        pth = os.path.join(amiga.extra, "status-now.txt")
        if os.path.exists(pth) and "Amatrix" not in open(pth, encoding="latin-1").read():
            break
        await asyncio.sleep(2)
    await asyncio.sleep(3)
    await amiga.rexx_raw(["address command 'Avail >Extra:avail-after.txt'",
                          "address command 'Status >Extra:status-after.txt'",
                          "if show('P', 'AMATRIX.1') then say 'port still there'"])
    for f in ("avail-before.txt", "avail-after.txt", "status-after.txt", "res.txt"):
        pth = os.path.join(amiga.extra, f)
        if os.path.exists(pth):
            for line in open(pth, encoding="latin-1").read().splitlines():
                ctx.diag(f"{f}: {line}")
    await amiga.rexx_raw(["address command 'Run >NIL: Extra:Amatrix >SYS:gui-out2.txt'"])
    await asyncio.sleep(25)
    await amiga.shot("12-unlock-window")
    rc, st = await amiga.rexx("STATUS")
    check("Passwort" in st, f"after a restart Amatrix asks for the passphrase ({st!r})")
    rc, _ = await amiga.rexx("UNLOCK falsch")

    async def wrong_done():
        rc, r = await amiga.rexx("STATUS")
        return r.startswith("Passwort") and r
    await asyncio.sleep(20)
    rc, st = await amiga.rexx("STATUS")
    check(not st.startswith("Online"), "wrong passphrase: still locked")
    rc, _ = await amiga.rexx("UNLOCK Tresor 42")
    st = await amiga.until(online, 300)
    check(st, f"right passphrase: online again with the saved login ({st})")
    if was_verified:
        check("verifiziert" in (st or ""), f"after a restart still verified ({st!r})")



async def scenario(ctx, amiga, srv, me):
    from nio import RoomCreateResponse
    check = ctx.check
    p = await ctx.peer()
    await p.c.set_displayname("Alice")
    await p.c.set_avatar(await upload(p, png(96, 96, (40, 90, 220)), "alice.png"))

    async def online():
        rc, r = await amiga.rexx("STATUS")
        return r if r.startswith("Online") else None
    await asyncio.sleep(45)
    await amiga.shot("00-start")
    st = await amiga.until(online, 300)
    check(st, f"client online ({st})")
    if os.environ.get("AMX_GUI_ONLY") == "store":
        await store_part(ctx, amiga, online)
        return
    await amiga.shot("01-online")

    # a direct chat and an encrypted group room with a picture, both invitations
    dm = await p.c.room_create(is_direct=True, invite=[me.user_id],
                               initial_state=[{"type": "m.room.encryption", "state_key": "",
                                               "content": {"algorithm": "m.megolm.v1.aes-sha2"}}])
    check(isinstance(dm, RoomCreateResponse), "peer: direct chat created")
    dm = dm.room_id
    room = await p.create_room(invite=[me.user_id], encrypted=True, name="Kaffeeklatsch")
    await p.c.room_put_state(room, "m.room.avatar", {"url": await upload(p, png(96, 96, (200, 120, 30)), "k.png")})

    async def invites():
        rc, r = await amiga.rexx("STATUS")
        return "invites=2" in r
    check(await amiga.until(invites, 180), "two invitations in the list")
    rc, v = await amiga.rexx("ROOMLINE Kaffeeklatsch")
    check(v == "Einladung von Alice", f"second line of an invitation names the inviter ({v!r})")
    await amiga.shot("02-invitations")

    rc1, _ = await amiga.rexx("ACCEPT Alice")
    rc2, _ = await amiga.rexx("ACCEPT Kaffeeklatsch")
    check(rc1 == "0" and rc2 == "0", f"accepted both (rc {rc1}, {rc2})")
    ok = await p.wait(lambda: p.joined(dm, me.user_id) and p.joined(room, me.user_id), 240)
    check(ok, "peer sees the client in both rooms")
    await p.refresh_devices(me.user_id)
    await p.send(dm, "Hallo Amiga! Schön, dass du da bist.")
    await p.send(room, "Wer hat Kuchen mitgebracht?")
    await p.sync(1000)

    async def lines_dm():
        rc, r = await amiga.rexx("OPENCHAT Alice", "CHATLINES Alice")
        return r.isdigit() and int(r) >= 2 and int(r)
    check(await amiga.until(lines_dm, 180), "chat window with Alice shows the messages")
    await amiga.shot("03-chat-alice")

    rc, _ = await amiga.rexx('SAY "Alice" Hallo Alice, hier schreibt der Amiga. Grüße! äöü')
    check(rc == "0", "typed a message (Return)")
    got = await p.wait(lambda: p.got(dm, me.user_id, "Hallo Alice, hier schreibt der Amiga. Grüße! äöü"), 180)
    check(got, "peer received it, decrypted, umlauts intact")
    await p.send(dm, "Wie geht es dir?")
    n0 = await lines_dm()

    async def more():
        v = await lines_dm()
        return v and n0 and v > n0
    check(await amiga.until(more, 120), "reply shows up live")


    async def preview(name, want):
        async def f():
            rc, r = await amiga.rexx("ROOMLINE " + name)
            return want in r and r
        return await amiga.until(f, 60)
    v = await preview("Alice", "Wie geht es dir?")
    check(v, f"room list, second line of Alice: newest message ({v!r})")
    v = await preview("Kaffeeklatsch", "Alice: Wer hat Kuchen mitgebracht?")
    check(v, f"second line of a group: sender and message ({v!r})")

    # emoji in the text: our pictures, unknown ones the yellow "?" face
    await p.send(dm, "Prima \U0001F602 und \U0001F984 und \u2764\ufe0f")

    async def emoji_line():
        rc, r = await amiga.rexx("LASTLINE Alice")
        return "Prima" in r and r
    v = await amiga.until(emoji_line, 120)
    check(v and "^o[4]" in v and "^o[9]" in v and "^o[2]" in v and "?" not in v.split(":", 1)[-1],
          f"emoji in a message shown as pictures ({v!r})")
    v = await preview("Alice", "Prima ")
    check(v and "?" not in v and v.count("\x1bA[") == 3, f"room list preview: emoji as pictures too ({v!r})")

    # a long message (wrapped over several lines), then close and reopen the
    # window a few times: freeing the lines must not crash (seen on the Vampire)
    long_msg = ("Das ist eine absichtlich sehr lange Nachricht, damit NList sie über mehrere "
                "Zeilen umbricht - genau dabei wurde beim Schließen des Fensters Speicher doppelt "
                "freigegeben. Noch ein paar Worte mehr, damit es auch im breiten Fenster umbricht.")
    n_before = await lines_dm()
    await p.send(dm, long_msg)

    async def got_long():
        v = await lines_dm()
        return v and n_before and v > n_before
    check(await amiga.until(got_long, 120), "long message shown (wrapped)")
    await amiga.shot("04b-long-message")
    ok = True
    for _ in range(3):
        rc1, _ = await amiga.rexx("CLOSECHAT Alice")
        await asyncio.sleep(4)
        rc2, _ = await amiga.rexx("OPENCHAT Alice")
        await asyncio.sleep(10)
        ok &= rc1 == "0" and rc2 == "0"
    rc, st = await amiga.rexx("STATUS")
    check(ok and st.startswith("Online"), f"closed and reopened the chat 3 times, still running ({st!r})")

    async def idle_status():
        rc, r = await amiga.rexx("CHATSTATUS Alice")
        return r == "Ende-zu-Ende-verschlüsselt" and r
    v = await amiga.until(idle_status, 60)
    check(v, f"status line of the chat window when idle ({v!r})")
    await amiga.shot("04-chat-reply")

    rc, _ = await amiga.rexx("OPENCHAT Kaffeeklatsch")
    check(rc == "0", "opened the group room")
    await asyncio.sleep(10)
    await amiga.shot("05-group")

    # reactions (under the message, grouped, with a count): Bob joins the
    # group; Alice and Bob react, so do we; Bob takes one back
    bob = await ctx.peer(ctx.bob)
    await bob.c.set_displayname("Bob")
    await p.invite(room, ctx.bob.user_id)
    rb = await bob.wait_invite(120)
    check(rb == room, "Bob joined the group")
    await p.wait_member(room, ctx.bob.user_id, 60)
    for x in (p, bob):
        await x.refresh_devices(me.user_id)
        await x.refresh_devices(ctx.bob.user_id if x is p else ctx.peer_user.user_id)
    kuchen = p.ids.get((room, ctx.peer_user.user_id, "Wer hat Kuchen mitgebracht?"))
    m2 = await bob.send(room, "Ich! Käsekuchen.")

    async def react(who, ev, key):
        r = await who.c.room_send(room, "m.reaction",
                                  {"m.relates_to": {"rel_type": "m.annotation", "event_id": ev, "key": key}},
                                  ignore_unverified_devices=True)
        return getattr(r, "event_id", None)
    await react(p, kuchen, "\U0001F44D")
    await react(bob, kuchen, "\U0001F44D\U0001F3FD")         # skin tone: same picture
    joy = await react(bob, kuchen, "\U0001F602")
    await react(p, kuchen, "\U0001F606")                      # another laughing face: same group
    await react(p, m2, "\u2764\ufe0f")
    await react(p, m2, "\U0001F618")                          # kiss: Alice again -> still 1 heart
    await react(p, m2, "\U0001F389")
    await react(p, m2, "\U0001F404")                          # no picture: shown as text

    async def reactions(want):
        async def f():
            rc, r = await amiga.rexx("REACTIONS Kaffeeklatsch")
            return want(r) and r
        return await amiga.until(f, 120)
    v = await reactions(lambda r: "thumbsup 2" in r and "joy 2" in r and "heart 1" in r and "tada 1" in r
                        and "other 1" in r)
    check(v, f"reactions under the messages, grouped and counted ({v!r})")
    rc, _ = await amiga.rexx("REACT Kaffeeklatsch heart")
    check(rc == "0", "our own reaction (newest message of someone else)")
    v = await reactions(lambda r: "heart 2*" in r)
    check(v, f"our heart counted and marked as ours ({v!r})")
    await asyncio.sleep(5)
    await amiga.shot("05c-reactions")
    await bob.c.room_redact(room, joy)
    v = await reactions(lambda r: "joy 1" in r and "thumbsup 2" in r)
    check(v, f"Bob took his reaction back ({v!r})")

    rc, _ = await amiga.rexx("NEWROOM Amiga-Runde")
    check(rc == "0", "new room command (no invitations in that window any more)")
    await asyncio.sleep(10)
    await amiga.shot("05b-new-room-toolbar")

    async def invite():
        rc, _ = await amiga.rexx(f"INVITE Amiga-Runde {ctx.peer_user.user_id}")
        return rc == "0"
    check(await amiga.until(invite, 120), "invited through the toolbar button of the room")
    new = await p.wait_invite(240)
    check(new is not None, "peer invited to the room created in the GUI")
    if new:
        await p.c.join(new)
        await p.sync(1000)
        await p.refresh_devices(me.user_id)
        await p.send(new, "Danke für die Einladung!")
    await asyncio.sleep(15)
    await amiga.shot("06-new-room")

    for name in ("Alice", "Kaffeeklatsch", "Amiga-Runde"):
        async def readable(name=name):
            rc, r = await amiga.rexx("UNREADABLE " + name)
            return r == "0"
        check(await amiga.until(readable, 120), f"{name}: every message decrypted (late keys replace the placeholder)")


    # a room with a long, mixed history from before we joined (like a real
    # account on a new device): nothing of it is decryptable for us
    alt = await p.create_room(encrypted=True, name="Altraum")
    await p.sync(500)
    ids = []
    for i in range(1, 36):
        r = await p.c.room_send(alt, "m.room.message",
                                {"msgtype": "m.text", "body": f"alt {i:02d}\nzweite Zeile",
                                 "format": "org.matrix.custom.html",
                                 "formatted_body": f"<b>alt</b> {i:02d}<br>zweite Zeile"},
                                ignore_unverified_devices=True)
        ids.append(getattr(r, "event_id", None))
    await p.c.room_send(alt, "m.reaction", {"m.relates_to": {"rel_type": "m.annotation",
                        "event_id": ids[0], "key": "👍"}}, ignore_unverified_devices=True)
    await p.c.room_send(alt, "m.room.message", {"msgtype": "m.text", "body": "* korrigiert",
                        "m.new_content": {"msgtype": "m.text", "body": "korrigiert"},
                        "m.relates_to": {"rel_type": "m.replace", "event_id": ids[1]}},
                        ignore_unverified_devices=True)
    await p.c.room_redact(alt, ids[2], reason="weg damit")
    await p.c.room_send(alt, "m.room.message", {"msgtype": "m.image", "body": "bild.png",
                        "url": "mxc://amatrix.test/nichtda", "info": {"w": 10, "h": 10}},
                        ignore_unverified_devices=True)
    await p.c.room_put_state(alt, "m.room.topic", {"topic": "Ein Thema mit Ümläuten"})
    await p.invite(alt, me.user_id)

    async def accept_alt():
        rc, _ = await amiga.rexx("ACCEPT Altraum")
        return rc == "0"
    check(await amiga.until(accept_alt, 180), "accepted Altraum (history from before we joined)")
    ok = await p.wait(lambda: p.joined(alt, me.user_id), 240)
    rc, _ = await amiga.rexx("OPENCHAT Altraum")
    await asyncio.sleep(25)
    rc, st = await amiga.rexx("STATUS")
    check(st.startswith("Online"), f"GUI still answers after opening Altraum ({st!r})")
    rc, n = await amiga.rexx("CHATLINES Altraum")
    check(n.isdigit() and int(n) > 0, f"Altraum shows its history ({n} lines)")
    await amiga.shot("07a-altraum")
    rc, _ = await amiga.rexx("OLDER Altraum")
    await asyncio.sleep(20)
    rc, st = await amiga.rexx("STATUS")
    check(st.startswith("Online"), f"GUI still answers after loading older messages ({st!r})")
    await amiga.shot("07b-altraum-older")

    # older messages: 45 in a room, the window loads the newest 30, the button the rest
    archiv = await p.create_room(invite=[me.user_id], encrypted=True, name="Archiv")
    async def accept_archiv():
        rc, _ = await amiga.rexx("ACCEPT Archiv")
        return rc == "0"
    check(await amiga.until(accept_archiv, 180), "accepted Archiv")
    ok = await p.wait(lambda: p.joined(archiv, me.user_id), 240)
    check(ok, "joined Archiv")
    await p.refresh_devices(me.user_id)
    for i in range(1, 46):
        await p.send(archiv, f"Nachricht {i:02d}")
    await p.sync(1000)
    await amiga.rexx("OPENCHAT Archiv")      # the window may have opened on accepting; reopen = front

    async def archiv_lines():
        rc, r = await amiga.rexx("CHATLINES Archiv")
        return int(r) if r.isdigit() else 0
    async def has45():
        rc, r = await amiga.rexx("FIRSTLINE Archiv")
        n = await archiv_lines()
        return n >= 45 and n
    n1 = await amiga.until(lambda: archiv_lines(), 180)
    await asyncio.sleep(20)                  # let live messages and the first history page settle
    n1 = await archiv_lines()
    rc, first1 = await amiga.rexx("FIRSTLINE Archiv")
    check(n1 > 0 and "Nachricht 01" not in first1,
          f"window opens with the newest messages, not all ({n1} lines, top: {first1!r})")
    await amiga.shot("08-archiv-before")
    rc, _ = await amiga.rexx("OLDER Archiv")
    check(rc == "0", "pressed 'Ältere Nachrichten laden'")
    n2 = await amiga.until(has45, 120)
    rc, first2 = await amiga.rexx("FIRSTLINE Archiv")
    check(n2 and "Nachricht 01" in first2, f"older messages loaded on top ({n2} lines, top: {first2!r})")
    async def archiv_readable():
        rc, r = await amiga.rexx("UNREADABLE Archiv")
        return r == "0"
    check(await amiga.until(archiv_readable, 120), "Archiv: no placeholder left after the keys arrived")
    for _ in range(6):                       # press until the button is greyed out
        rc, _ = await amiga.rexx("OLDER Archiv")
        if rc == "10":
            break
        await asyncio.sleep(15)
    check(rc == "10", f"at the beginning of the room the button is greyed out (rc {rc})")
    for name in ("Archiv", "Alice", "Amiga-Runde"):
        async def read0(name=name):
            rc, r = await amiga.rexx("UNREAD " + name)
            return r == "0"
        check(await amiga.until(read0, 120), f"{name}: open window marks it read (no unread count)")
    await amiga.shot("09-archiv-after")

    # security key: a second device of our user (like Element) with old
    # messages only the key backup can decrypt
    import importlib.util
    spec = importlib.util.spec_from_file_location("recovery_peer", os.path.join(TESTS, "features", "recovery", "peer.py"))
    rec = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rec)
    info = await rec.element_device(ctx, me)
    check(info is not None, "Element-like second device of our user set up")
    if info:
        async def open_old():
            rc, _ = await amiga.rexx("OPENCHAT Vor meiner Zeit")
            return rc == "0"
        check(await amiga.until(open_old, 180), "room of the other device opened")
        await asyncio.sleep(20)
        rc, u = await amiga.rexx("UNREADABLE Vor meiner Zeit")
        check(u.isdigit() and int(u) >= 5, f"before the key: messages not readable ({u} placeholders)")
        await amiga.shot("10-before-key")
        rc, _ = await amiga.rexx("RECOVERY " + rec.RECOVERY_KEY)
        check(rc == "0", "security key entered")

        async def verified():
            rc, r = await amiga.rexx("STATUS")
            return "verifiziert" in r
        check(await amiga.until(verified, 300), "status says verified")

        async def readable():
            rc, r = await amiga.rexx("UNREADABLE Vor meiner Zeit")
            return r == "0"
        check(await amiga.until(readable, 240), "old messages decrypted from the key backup")
        rc, first = await amiga.rexx("FIRSTLINE Vor meiner Zeit")
        rc, n = await amiga.rexx("CHATLINES Vor meiner Zeit")
        ctx.diag(f"Vor meiner Zeit: {n} lines, first {first!r}")
        async def preview_ok():
            rc, r = await amiga.rexx("ROOMLINE Vor meiner Zeit")
            return "aus der Zeit vor Amatrix 5" in r and r
        v = await amiga.until(preview_ok, 60)
        check(v, f"room list preview decrypted too ({v!r})")
        await amiga.shot("11-after-key")
        check(await rec.wait_signed(ctx, info, 120), "Element would show the Amiga device as verified")

    await store_part(ctx, amiga, online, bool(info))

    rc, _ = await amiga.rexx("NEWCHAT " + ctx.peer_user.user_id)
    check(rc == "0", "new chat with an existing partner opens the existing chat")
    await asyncio.sleep(5)
    await amiga.shot("07-end")

    # leave the room created in the GUI for good: window closes, room gone
    if new:
        rc, _ = await amiga.rexx("LEAVE Amiga-Runde")
        check(rc == "0", "left Amiga-Runde (toolbar door / menu, without the question)")
        check(await p.wait_left(new, me.user_id, 120), "peer sees us leave")

        async def gone():
            rc, r = await amiga.rexx("ROOMLINE Amiga-Runde")
            return rc != "0"
        check(await amiga.until(gone, 60), "Amiga-Runde is no longer in the room list")


async def main():
    import logging
    logging.basicConfig(level=logging.ERROR)
    for n in ("nio", "peewee", "asyncio"):
        logging.getLogger(n).setLevel(logging.ERROR)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    r = subprocess.run(["make", "-j4", "TARGET=amiga", "gui"], cwd=TOP, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:])
        return 2
    ensure_venv()
    os.makedirs(SHOTS, exist_ok=True)
    for f in os.listdir(SHOTS):
        os.remove(os.path.join(SHOTS, f))
    logpath = os.path.join(TOP, "build", "amiga", "gui-test.log")
    logf = open(logpath, "w", encoding="utf-8")

    def log(line):
        logf.write(line + "\n")
        logf.flush()
        if a.verbose or line.startswith("not ok"):
            print(line, flush=True)

    with TestServer() as srv:
        sysdir = os.path.join(TESTENV, "system")
        if not os.path.isdir(os.path.join(sysdir, "MUI")):
            print("MUI missing in the emulator: run tests/emu/setup-emu-mui.sh")
            return 2
        shutil.copy(srv.ca_pem, os.path.join(sysdir, "AmiSSL", "Certs", srv.ca_hash_name()))
        me, other, bob = srv.register("amiga"), srv.register("peer"), srv.register("bob")
        extra = tempfile.mkdtemp(prefix="amx-gui-")
        shutil.copy(os.path.join(TOP, "build", "amiga", "gui", "Amatrix"), os.path.join(extra, "Amatrix"))
        script = os.path.join(extra, ".autotest")
        v = os.environ.get("AMX_VERBOSE")
        with open(script, "w", encoding="latin-1") as f:
            f.write(LOOP.format(server=srv.https_url, user=me.localpart, password=me.password,
                                verbose=f"SetEnv AMX_VERBOSE {v}" if v else "UnSetEnv AMX_VERBOSE"))
        for fn in ("gui-out.txt",):
            if os.path.exists(os.path.join(sysdir, fn)):
                os.remove(os.path.join(sysdir, fn))
        emu = await asyncio.create_subprocess_exec(
            os.path.join(TOP, "tests", "emu", "run-amiga-gui.sh"), script, "1800", extra, SHOTS,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)

        from peerlib import Context
        workdir = tempfile.mkdtemp(prefix="amx-gui-peer-")
        ctx = Context(srv.http_url, other, me, workdir, log)
        ctx.scale = 3
        ctx.bob = bob
        amiga = Amiga(extra, log)
        t0 = time.time()
        try:
            await asyncio.wait_for(scenario(ctx, amiga, srv, me), 1600)
        except asyncio.TimeoutError:
            ctx.check(False, "scenario finished in time")
        except Exception as e:
            ctx.check(False, f"scenario raised {type(e).__name__}: {e}")
        finally:
            rc, _ = await amiga.rexx("QUIT", secs=60)
            await asyncio.sleep(8)
            open(os.path.join(extra, "stop"), "w").close()
            await ctx.close()
        out, _ = await emu.communicate()
        gui_out = os.path.join(sysdir, "gui-out.txt")
        if os.path.exists(gui_out):
            for line in open(gui_out, encoding="latin-1").read().splitlines():
                log(f"# amatrix: {line}")
        alive = os.path.exists(os.path.join(extra, "Amatrix"))
        ctx.check(alive, "program still there at the end")
        for line in out.decode(errors="replace").splitlines():
            log(f"# emu: {line}")
        shutil.rmtree(extra, ignore_errors=True)
        shutil.rmtree(workdir, ignore_errors=True)
    bad = [n for ok, n in ctx.results if not ok]
    print(f"GUI test: {len(ctx.results) - len(bad)}/{len(ctx.results)} checks ok in {time.time() - t0:.0f}s; "
          f"log {logpath}, screenshots {SHOTS}")
    return 1 if bad else 0


if __name__ == "__main__":
    try:
        import nio  # noqa: F401
    except ImportError:
        py = ensure_venv()
        os.execv(py, [py] + sys.argv)
    sys.exit(asyncio.run(main()))
