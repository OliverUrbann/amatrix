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
import argparse, asyncio, io, os, re, shutil, subprocess, sys, tempfile, time

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
{herald}
Run >NIL: Extra:Amatrix >SYS:gui-out.txt SERVER={server} USER={user} PASSWORD={password} DEBUG
Lab loop
If EXISTS Extra:go
  Delete Extra:go QUIET
  rx Extra:c.rexx >Extra:res.txt
  Echo "" >Extra:ok
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

    async def settle(self, secs=300):
        """Waits until the script started before has finished (Extra:ok)."""
        ok = os.path.join(self.extra, "ok")
        end = time.time() + secs
        while getattr(self, "running", False) and time.time() < end:
            if os.path.exists(ok):
                self.running = False
                break
            await asyncio.sleep(0.5)

    async def go(self, secs=60):
        """Lets the Amiga run Extra:c.rexx (output to Extra:res.txt); waits until done.  A
        script that takes longer is waited for before the next one is written."""
        ok = os.path.join(self.extra, "ok")
        if os.path.exists(ok):
            os.remove(ok)
        self.running = True
        open(os.path.join(self.extra, "go"), "w").close()
        end = time.time() + secs
        while time.time() < end:
            if os.path.exists(ok):
                self.running = False
                return True
            await asyncio.sleep(0.5)
        self.log(f"# amiga: rexx script not done in {secs}s")
        self.timeouts = getattr(self, "timeouts", 0) + 1
        await self.shot(f"timeout-{self.timeouts}")
        return False

    async def rexx(self, *cmds, secs=60):
        """Sends ARexx commands to AMATRIX; returns RC and RESULT of the last."""
        await self.settle()
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
        await self.settle()
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
    if os.environ.get("AMX_GUI_ONLY") != "store":       # the history cache: messages at once
        rc, _ = await amiga.rexx("OPENCHAT Alice", "CHATCACHED Alice")

        async def cached():
            rc, r = await amiga.rexx("CHATCACHED Alice")
            return r.isdigit() and int(r) > 0 and r
        v = await amiga.until(cached, 60)
        check(v, f"after the restart the chat shows cached messages first ({v!r})")

        async def read_again():
            rc, r = await amiga.rexx("READ Alice")
            return r.isdigit() and int(r) >= 1 and r
        v = await amiga.until(read_again, 60)
        check(v, f"after the restart: check marks on our read messages ({v!r})")



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
    await asyncio.sleep(3)                  # our own message's echo is in
    rc, _ = await amiga.rexx("COPY Alice")
    rc2, v = await amiga.rexx("CLIP")
    check(rc == "0" and v == "Hallo Alice, hier schreibt der Amiga. Grüße! äöü",
          f"newest message copied to the clipboard, umlauts in Latin-1 ({v!r})")

    # link preview (the server fetches the page; in encrypted rooms only when switched on)
    import importlib.util
    spec = importlib.util.spec_from_file_location("lp", os.path.join(TOP, "tests", "features", "link_preview", "peer.py"))
    lp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lp)
    web = lp.serve()
    await amiga.rexx("SETOPTION linkprev_enc 1")
    await p.send(dm, f"Hier: http://127.0.0.1:{web.server_port}/seite.html")

    async def preview_line():
        rc, r = await amiga.rexx("PREVIEWS Alice")
        return "Amiga-Seite" in r and r
    v = await amiga.until(preview_line, 90)
    check(v and "Alles über den Amiga" in v, f"link preview under the message ({v!r})")
    rc, r = await amiga.rexx("ATBOTTOM Alice")
    check(r == "1", f"after the link preview the newest line is still in view ({r!r})")
    await amiga.shot("03d-link-preview")
    web.shutdown()

    # polls: Alice asks, we vote, she votes and ends; our own poll
    from nio import UnknownEvent
    unknown = []
    p.c.add_event_callback(lambda r_, ev: unknown.append(ev.source), UnknownEvent)
    pr = await p.c.room_send(dm, "org.matrix.msc3381.poll.start", {
        "org.matrix.msc3381.poll.start": {
            "question": {"org.matrix.msc1767.text": "Pizza oder Pasta?"},
            "kind": "org.matrix.msc3381.poll.disclosed", "max_selections": 1,
            "answers": [{"id": "p1", "org.matrix.msc1767.text": "Pizza"},
                        {"id": "p2", "org.matrix.msc1767.text": "Pasta"}]},
        "org.matrix.msc1767.text": "Pizza oder Pasta?"}, ignore_unverified_devices=True)

    async def rows(want):
        rc, r = await amiga.rexx("POLLROWS Alice")
        return all(w_ in r for w_ in want) and r
    v = await amiga.until(lambda: rows(["Pizza", "Pasta", "0 Stimmen"]), 90)
    check(v, f"poll shown with its answers ({v!r})")
    await amiga.rexx("VOTE Alice 1")
    ok = await p.wait(lambda: any(e.get("type") == "org.matrix.msc3381.poll.response" and
                                  e["content"].get("org.matrix.msc3381.poll.response", {}).get("answers") == ["p2"]
                                  for e in unknown), 90)
    check(ok, "our vote (Pasta) reaches Alice")
    ref = {"rel_type": "m.reference", "event_id": pr.event_id}
    await p.c.room_send(dm, "org.matrix.msc3381.poll.response",
                        {"m.relates_to": ref, "org.matrix.msc3381.poll.response": {"answers": ["p1"]}},
                        ignore_unverified_devices=True)
    await p.c.room_send(dm, "org.matrix.msc3381.poll.end",
                        {"m.relates_to": ref, "org.matrix.msc3381.poll.end": {}, "org.matrix.msc1767.text": "Ende"},
                        ignore_unverified_devices=True)
    v = await amiga.until(lambda: rows(["Pizza\33n  (1)", "Pasta\33n  (1) <X>", "2 Stimmen - beendet"]), 90)
    rc, now = await amiga.rexx("POLLROWS Alice")
    check(v, f"her vote counted, our choice marked, poll ended ({v!r}; now {now!r})")
    await amiga.shot("03e-poll")
    await amiga.rexx('POLL Alice "Welcher Amiga?" "A500|A1200|Vampire"')
    ok = await p.wait(lambda: any(e.get("type") == "org.matrix.msc3381.poll.start" and
                                  len(e["content"]["org.matrix.msc3381.poll.start"]["answers"]) == 3
                                  for e in unknown), 90)
    check(ok, "our own poll reaches Alice with three answers")

    # slash commands
    from nio import RoomMessageEmote, RoomMessageText
    seen_src = []
    p.c.add_event_callback(lambda r_, ev: seen_src.append(ev.source), (RoomMessageEmote, RoomMessageText))
    await amiga.rexx("SAY Alice /me winkt")
    ok = await p.wait(lambda: any(e["content"].get("msgtype") == "m.emote" and e["content"].get("body") == "winkt"
                                  for e in seen_src), 60)
    check(ok, "/me winkt: an emote reaches Alice")
    await amiga.rexx("SAY Alice /spoiler Geheimnis")
    ok = await p.wait(lambda: any("data-mx-spoiler" in e["content"].get("formatted_body", "") for e in seen_src), 60)
    check(ok, "/spoiler: sent as a spoiler")
    await amiga.rexx("SAY Alice //nicht als Befehl")
    check(await p.wait_message(dm, me.user_id, "/nicht als Befehl", 60), "//text: sent with one slash")
    await amiga.rexx(f"SAY Alice /whois {p.user.user_id}")

    async def whois():
        rc, r = await amiga.rexx("CHATSTATUS Alice")
        return "Alice" in r and r
    v = await amiga.until(whois, 60)
    check(v, f"/whois: her name in the status line ({v!r})")
    # markdown, :emoji: codes, completing @names (a mention for Element)
    await amiga.rexx("SAY Alice **fett** und :thumbsup:")
    ok = await p.wait(lambda: any(e["content"].get("formatted_body") == "<strong>fett</strong> und \U0001F44D"
                                  and e["content"].get("body") == "**fett** und \U0001F44D" for e in seen_src), 60)
    check(ok, "markdown sent as HTML, :thumbsup: as the emoji")

    async def suggest():
        await amiga.rexx("TYPE Alice Hallo @Al")
        rc, r = await amiga.rexx("SUGGEST Alice")
        return r == "Alice" and r
    v = await amiga.until(suggest, 60)
    check(v, f"typing @Al suggests Alice ({v!r})")
    await amiga.shot("03f-suggest")
    await amiga.rexx("COMPLETE Alice 0")
    await amiga.rexx("SENDINPUT Alice")
    ok = await p.wait(lambda: any(p.user.user_id in e["content"].get("m.mentions", {}).get("user_ids", [])
                                  and "matrix.to/#/" + p.user.user_id in e["content"].get("formatted_body", "")
                                  for e in seen_src), 60)
    check(ok, "completed name sent as a mention (pill and m.mentions)")
    await amiga.rexx("SAY Alice /gibtsnicht")
    rc, v = await amiga.rexx("CHATSTATUS Alice")
    check("Unbekannter Befehl" in v, f"unknown command: a hint, nothing sent ({v!r})")
    await amiga.rexx("TYPE Alice")

    # display options, drafts, cursor up edits our newest message
    await amiga.rexx("SETOPTION 24h 0", secs=300)
    rc, v = await amiga.rexx("LASTLINE Alice")
    check(re.search(r"\d\d:\d\d(AM|PM)", v or ""), f"12-hour clock ({v!r})")
    await amiga.rexx("SETOPTION 24h 1", secs=300)
    for _ in range(5):                      # right after re-rendering all windows the Amiga is busy
        await amiga.rexx("TYPE Alice Ein Entwurf")
        rc0, inp = await amiga.rexx("INPUT Alice")
        if inp == "Ein Entwurf":
            break
        await asyncio.sleep(3)
    await amiga.rexx("CLOSECHAT Alice")

    async def closed():
        rc, r = await amiga.rexx("CHATSTATUS Alice")
        return rc != "0"
    await amiga.until(closed, 30)
    ctx.diag(f"input before closing: {inp!r}")
    rc, v = await amiga.rexx("DRAFT Alice")
    rc2, line = await amiga.rexx("ROOMLINE Alice")
    check(v == "Ein Entwurf" and "Entwurf" in line, f"closed with text: kept as a draft, shown in the list ({v!r}, {line!r})")
    await amiga.rexx("OPENCHAT Alice")
    await asyncio.sleep(4)
    rc, v = await amiga.rexx("INPUT Alice")
    check(v == "Ein Entwurf", f"opened again: the draft is back in the input ({v!r})")
    await amiga.rexx("TYPE Alice")
    await amiga.rexx("EDITUP Alice")
    rc, v = await amiga.rexx("INPUT Alice")
    rc2, st = await amiga.rexx("CHATSTATUS Alice")
    check(v and v != "Ein Entwurf", f"cursor up: our newest message in the input to edit ({v!r})")
    for _ in range(2):                              # out of edit mode, then the draft away (empty input)
        await amiga.rexx("CLOSECHAT Alice")
        await asyncio.sleep(2)
        await amiga.rexx("OPENCHAT Alice")
        await asyncio.sleep(4)
        await amiga.rexx("TYPE Alice")
    rc, v = await amiga.rexx("DRAFT Alice")
    check(v == "", f"emptied: no draft left ({v!r})")

    # typing and read receipts
    from nio import TypingNoticeEvent
    peer_typing = []
    p.c.add_ephemeral_callback(lambda r_, ev: peer_typing.append(list(ev.users)), TypingNoticeEvent)
    await p.c.room_typing(dm, True, 30000)

    async def typing():
        rc, r = await amiga.rexx("TYPING Alice")
        return r == "Alice" and r
    check(await amiga.until(typing, 60), "\"Alice tippt ...\" in the chat window")
    await amiga.shot("03b-typing")
    await p.c.room_typing(dm, False)

    async def no_typing():
        rc, r = await amiga.rexx("TYPING Alice")
        return rc == "0" and r == ""
    check(await amiga.until(no_typing, 60), "and gone when she stops")
    await amiga.rexx("TYPE Alice Ich schreibe gerade")
    ok = await p.wait(lambda: any(me.user_id in t for t in peer_typing), 60)
    check(ok, "typing in the input field: Alice sees us typing")
    await amiga.rexx('TYPE Alice')
    ok = await p.wait(lambda: peer_typing and me.user_id not in peer_typing[-1], 60)
    check(ok, "input emptied: typing stopped")
    eid = p.ids.get((dm, me.user_id, "Hallo Alice, hier schreibt der Amiga. Grüße! äöü"))
    if eid:
        await p.c.room_read_markers(dm, eid, eid)

    async def read():
        rc, r = await amiga.rexx("READ Alice")
        return r.isdigit() and int(r) >= 1 and r
    check(eid and await amiga.until(read, 60), "Alice read our message: a check mark")

    async def readers():
        rc, r = await amiga.rexx("READERS Alice")
        return "gelesen von Alice" in r and r
    v = await amiga.until(readers, 60)
    check(v, f"and \"gelesen von Alice\" under it ({v!r})")
    rc, v = await amiga.rexx("ATBOTTOM Alice")
    check(v == "1", f"the window still shows the newest line ({v!r})")
    await amiga.shot("03c-read")

    # menus: items that need a marked message / a person are greyed out otherwise
    await amiga.rexx("MARK Alice Hallo Alice, hier schreibt der Amiga")      # our own message
    rc, quote_on = await amiga.rexx("MENUON Zitieren")
    rc, dm_own = await amiga.rexx("MENUON Direktnachricht")
    rc, kick_own = await amiga.rexx("MENUON Aus dem Raum werfen")
    check(quote_on == "1" and dm_own == "0" and kick_own == "0",
          f"menu: own message can be quoted, but is no person ({quote_on!r}, {dm_own!r}, {kick_own!r})")
    await amiga.rexx("MARK Alice Hier: http")                                # Alice's message
    rc, dm_on = await amiga.rexx("MENUON Direktnachricht")
    rc, men_on = await amiga.rexx("MENUON Erw\xe4hnen")
    rc, ign_on = await amiga.rexx("MENUON Ignorieren")
    rc, edh = await amiga.rexx("MENUON Bearbeitungsverlauf")
    check(dm_on == "1" and men_on == "1" and ign_on == "1" and edh == "0",
          f"menu: person items usable for Alice's message, edit history not ({dm_on!r}, {men_on!r}, {ign_on!r}, {edh!r})")
    await amiga.rexx("ACTION Alice mention")
    rc, typed = await amiga.rexx("INPUT Alice")
    check("Alice" in typed, f"menu Person/Erw\xe4hnen puts the name into the input ({typed!r})")
    await amiga.rexx("ACTION Alice copyid")
    rc, clip = await amiga.rexx("CLIP")
    check(clip.startswith("@") and ":" in clip, f"menu Person/Benutzer-ID kopieren ({clip!r})")
    await amiga.rexx("TYPE Alice")
    # reporting a message to the admins
    await amiga.rexx("ACTION Alice report Spam")

    async def reported():
        rc, r = await amiga.rexx("CHATSTATUS Alice")
        return "Gemeldet" in r and r
    v = await amiga.until(reported, 60)
    check(v, f"message reported to the server's admins ({v!r})")

    # a public room before joining: what it is, its history
    from nio import RoomCreateResponse as RCR2, RoomVisibility, RoomPreset
    pubr = await p.c.room_create(visibility=RoomVisibility.public, alias="lesesaal", name="Lesesaal",
                                 preset=RoomPreset.public_chat,
                                 initial_state=[{"type": "m.room.history_visibility", "state_key": "",
                                                 "content": {"history_visibility": "world_readable"}}])
    if isinstance(pubr, RCR2):
        await p.sync(1000)
        await p.send(pubr.room_id, "Willkommen im Lesesaal")
        await amiga.rexx("SUMMARY #lesesaal:" + p.user.user_id.split(":", 1)[1])

        async def info():
            rc, r = await amiga.rexx("PUBINFO")
            return "ohne Beitritt lesbar" in r and r
        v = await amiga.until(info, 60)
        check(v, f"directory: what the room is before joining ({v!r})")
        await amiga.rexx("OPENPREVIEW " + pubr.room_id)

        async def prev_line():
            rc, r = await amiga.rexx("LASTLINE Vorschau")
            return "Willkommen im Lesesaal" in r and r
        v = await amiga.until(prev_line, 90)
        rc, st = await amiga.rexx("CHATSTATUS Vorschau")
        rc2, ll = await amiga.rexx("LASTLINE Vorschau")
        check(v, f"preview window shows its history without joining ({v!r}; status rc {rc} {st!r}, last rc {rc2} {ll!r})")
        await amiga.shot("03e-preview")
    n0 = await lines_dm()
    await p.send(dm, "Wie geht es dir?")

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

    # pictures, files and links
    import io as _io
    from PIL import Image as _Image
    # 128x96 (shown 1:1): left red, right blue, a black line every 8 rows -
    # the strips must join without gaps
    pic = _Image.new("RGB", (128, 96), (220, 30, 30))
    for y in range(96):
        for x in range(128):
            if y % 8 == 0:
                pic.putpixel((x, y), (0, 0, 0))
            elif x >= 64:
                pic.putpixel((x, y), (30, 30, 220))
    buf = _io.BytesIO()
    pic.save(buf, "PNG")
    data = buf.getvalue()

    async def up(d, mime, name):
        resp, keys = await p.c.upload(lambda *a: _io.BytesIO(d), content_type=mime, filename=name,
                                      encrypt=True, filesize=len(d))
        return {"url": resp.content_uri, "key": keys["key"], "iv": keys["iv"], "hashes": keys["hashes"], "v": "v2"}
    await p.c.room_send(dm, "m.room.message", {"msgtype": "m.image", "body": "rot-blau.png",
                        "file": await up(data, "image/png", "rot-blau.png"),
                        "info": {"mimetype": "image/png", "size": len(data), "w": 128, "h": 96}},
                        ignore_unverified_devices=True)
    txt = "Hallo vom Anhang\n".encode()
    await p.c.room_send(dm, "m.room.message", {"msgtype": "m.file", "body": "notiz.txt",
                        "file": await up(txt, "text/plain", "notiz.txt"),
                        "info": {"mimetype": "text/plain", "size": len(txt)}}, ignore_unverified_devices=True)
    await p.send(dm, "Schau mal https://www.amiga.org/forum. und www.aminet.net")

    async def shown():
        rc, r = await amiga.rexx("MEDIA Alice")
        return "rot-blau.png:2:" in r and "notiz.txt" in r and r
    v = await amiga.until(shown, 180)
    check(v, f"picture shown as a preview in strips, file listed ({v!r})")
    await asyncio.sleep(2)
    rc, r = await amiga.rexx("ATBOTTOM Alice")
    check(r == "1", f"the picture came later: the newest line is still in view ({r!r})")
    rc, v = await amiga.rexx("LINKS Alice")
    check(v.endswith(" https://www.amiga.org/forum http://www.aminet.net") or v == "https://www.amiga.org/forum http://www.aminet.net",
          f"links found, sentence end left out ({v!r})")
    await asyncio.sleep(3)
    await amiga.shot("04c-picture-link")
    shot = _Image.open(os.path.join(SHOTS, "04c-picture-link.png")).convert("RGB")
    isred = lambda q: q[0] > 150 and q[1] < 60 and q[2] < 60
    cols = {}
    for y in range(shot.height):
        for x in range(0, shot.width, 2):
            if isred(shot.getpixel((x, y))):
                cols[x] = cols.get(x, 0) + 1
    rows = []
    if cols:
        x0 = max(cols, key=cols.get)                 # the column through the picture
        for y in range(shot.height):
            q = shot.getpixel((x0, y))
            rows.append("k" if max(q) < 60 else "r" if isred(q) else "g")
    seq = "".join(rows)
    inner = seq[seq.find("kr"):seq.find("kr") + 96] if "kr" in seq else ""
    check(len(inner) == 96 and inner.count("k") >= 11 and inner.count("g") <= 3,
          f"preview strips in order, all 96 rows (known: now and then 1 row missing at a strip's end) ({seq})")
    # no DefIcons/Workbench and no PNG datatype in the emulator: the text file,
    # opened with MultiView (the fallback), shows the way works
    rc, _ = await amiga.rexx("OPENMEDIA Alice 1")
    check(rc == "0", "attachment opened (OPENMEDIA)")
    await asyncio.sleep(15)
    await amiga.shot("04d-picture-opened")
    target = os.path.join(amiga.extra, "notiz.txt")
    if os.path.exists(target):
        os.remove(target)
    rc, _ = await amiga.rexx("SAVEMEDIA Alice 1 Extra:notiz.txt")

    async def saved():
        return os.path.exists(target) and open(target, "rb").read() == txt
    check(await amiga.until(saved, 120), "file downloaded and decrypted (SAVEMEDIA)")

    # a voice message: label with its length
    import io as io_
    up, _ = await p.c.upload(io_.BytesIO(b"OggS" + bytes(200)), content_type="audio/ogg", filename="stimme.ogg",
                             filesize=204)
    await p.c.room_send(dm, "m.room.message", {
        "msgtype": "m.audio", "body": "stimme.ogg", "url": up.content_uri,
        "info": {"mimetype": "audio/ogg", "duration": 12000, "size": 204},
        "org.matrix.msc3245.voice": {}, "org.matrix.msc1767.audio": {"duration": 12000}},
        ignore_unverified_devices=True)

    async def voice():
        rc, r = await amiga.rexx("LASTLINE Alice")
        return "Sprachnachricht 0:12" in r and r
    v = await amiga.until(voice, 90)
    check(v, f"voice message shown with its length ({v!r})")


    # sending: a picture (as if chosen in the requester / dropped as an icon)
    # and an Amiga file without extension
    from nio import RoomEncryptedImage, RoomEncryptedFile, DownloadResponse
    from nio.crypto.attachments import decrypt_attachment
    sent = {}

    def on_att(room_, ev):
        if room_.room_id == dm:
            sent[ev.body] = ev
    p.c.add_event_callback(on_att, RoomEncryptedImage)
    p.c.add_event_callback(on_att, RoomEncryptedFile)
    pic = png(160, 120, (30, 160, 60))
    open(os.path.join(amiga.extra, "Gruen.png"), "wb").write(pic)
    rdm = b"Amatrix sendet eine Datei\n" * 50
    open(os.path.join(amiga.extra, "LiesMich"), "wb").write(rdm)
    rc1, _ = await amiga.rexx("SENDFILE Alice Extra:Gruen.png")
    rc2, _ = await amiga.rexx("SENDFILE Alice Extra:LiesMich")
    check(rc1 == "0" and rc2 == "0", f"two files sent (SENDFILE rc {rc1}, {rc2})")
    ok = await p.wait(lambda: "Gruen.png" in sent and "LiesMich" in sent, 180)
    check(ok, f"peer got both ({sorted(sent)})")

    async def fetched(name):
        ev = sent.get(name)
        if not ev:
            return None
        r = await p.c.download(mxc=ev.url)
        if not isinstance(r, DownloadResponse):
            return None
        f = ev.source["content"]["file"]
        return decrypt_attachment(r.body, f["key"]["k"], f["hashes"]["sha256"], f["iv"])
    if ok:
        c1 = sent["Gruen.png"].source["content"]
        check(await fetched("Gruen.png") == pic and c1["msgtype"] == "m.image"
              and c1["info"].get("w") == 160 and c1["info"].get("h") == 120,
              f"picture arrives intact as m.image 160x120 ({c1.get('msgtype')}, {c1.get('info')})")
        c2 = sent["LiesMich"].source["content"]
        check(await fetched("LiesMich") == rdm and c2["msgtype"] == "m.file",
              f"file arrives intact as m.file ({c2.get('msgtype')})")

    async def own_media():
        rc, r = await amiga.rexx("MEDIA Alice")
        return "Gruen.png" in r and "LiesMich" in r and r
    v = await amiga.until(own_media, 120)
    check(v, f"our own attachments show up in the chat ({v!r})")
    rc, _ = await amiga.rexx("SENDFILE Alice Extra:gibtsnicht")
    await asyncio.sleep(8)
    rc, v = await amiga.rexx("CHATSTATUS Alice")
    check("nicht gesendet" in v, f"a missing file: error in the status line ({v!r})")
    await amiga.shot("04e-sent-files")

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

    # formatted messages (HTML from Element) and mentions
    await p.c.room_send(dm, "m.room.message", {
        "msgtype": "m.text", "body": "**Wichtig:** _bitte_ lesen\n- eins\n- zwei",
        "format": "org.matrix.custom.html",
        "formatted_body": "<p><strong>Wichtig:</strong> <em>bitte</em> lesen</p>\n<ul>\n<li>eins</li>\n<li>zwei</li>\n</ul>"},
        ignore_unverified_devices=True)
    await p.c.room_send(dm, "m.room.message", {
        "msgtype": "m.text", "body": f"Hallo {me.user_id}, Code: `ls`",
        "format": "org.matrix.custom.html",
        "formatted_body": f'Hallo <a href="https://matrix.to/#/{me.user_id}">amiga</a>, Code: <code>ls</code>'
                          '<script>x</script>\x1bo[3@1]',
        "m.mentions": {"user_ids": [me.user_id]}}, ignore_unverified_devices=True)

    async def rich(want):
        async def f():
            rc, r = await amiga.rexx("TEXTS Alice 6")
            return want(r) and r
        return await amiga.until(f, 120)
    v = await rich(lambda r: "^bWichtig:^n ^ibitte^n lesen|- eins|- zwei" in r)
    check(v, f"formatted message: bold, italic, list ({v!r})")
    v = await rich(lambda r: "^b@^n " in r and "Hallo ^bamiga^n, Code: `ls`" in r)
    check(v and "^o[3@1]" not in v, f"mention marked, pill bold, no codes from the sender ({v!r})")
    await asyncio.sleep(2)
    await amiga.shot("04g-formatted")

    # replies, edits, deleting, date separator
    async def texts(want, n=12):
        async def f():
            rc, r = await amiga.rexx(f"TEXTS Alice {n}")
            return want(r) and r
        return await amiga.until(f, 120)
    v = await texts(lambda r: "^c^i- " in r and " 2026 -" in r or " 2027 -" in r, 400)
    check(v, "a date separator above the day's messages")
    rc, _ = await amiga.rexx("REPLY Alice Klar doch")
    check(rc == "0", "replied to the newest message of Alice")
    ok = await p.wait(lambda: p.got(dm, me.user_id, "Klar doch"), 120)
    ev = next((e for e in p.events if getattr(e, "body", None) == "Klar doch"), None)
    rel = ((ev.source.get("content") or {}).get("m.relates_to") or {}) if ev else {}
    check(ok and rel.get("m.in_reply_to", {}).get("event_id"), "Alice gets it as a reply")
    v = await texts(lambda r: "^i  \xbb Alice: Hallo" in r and "Klar doch" in r)
    check(v, f"our reply shows the quote above it ({v!r})")
    mine = p.ids.get((dm, me.user_id, "Klar doch"))
    await p.c.room_send(dm, "m.room.message", {
        "msgtype": "m.text", "body": f"> <{me.user_id}> Klar doch\n\nSchön",
        "m.relates_to": {"m.in_reply_to": {"event_id": mine}}}, ignore_unverified_devices=True)
    v = await texts(lambda r: "Klar doch|" in r and "Alice:^n Schön" in r and "> <" not in r)
    check(v, f"Alice's reply: quote, without the old fallback text ({v!r})")
    rc, _ = await amiga.rexx("EDITLAST Alice Klar doch, gerne")
    v = await texts(lambda r: "Klar doch, gerne ^i(bearbeitet)" in r)
    check(v, f"our edit shown in place, marked (bearbeitet) ({v!r})")
    parts = v.split("|") if v else []
    qi = next((i for i, t in enumerate(parts) if "\xbb Alice: Hallo" in t), -1)
    ti = next((i for i, t in enumerate(parts) if "Klar doch, gerne" in t), -1)
    check(0 <= qi and qi + 1 == ti, f"after it was rebuilt, the quote is still right above our reply ({qi}, {ti})")
    ok = await p.wait(lambda: any(((e.source.get("content") or {}).get("m.relates_to") or {}).get("rel_type")
                                  == "m.replace" for e in p.events), 120)
    check(ok, "Alice gets the edit")
    schoen = p.ids.get((dm, ctx.peer_user.user_id, f"> <{me.user_id}> Klar doch\n\nSchön"))
    await p.c.room_send(dm, "m.room.message", {
        "msgtype": "m.text", "body": "* Sehr schön", "m.new_content": {"msgtype": "m.text", "body": "Sehr schön"},
        "m.relates_to": {"rel_type": "m.replace", "event_id": schoen}}, ignore_unverified_devices=True)
    v = await texts(lambda r: "Sehr schön ^i(bearbeitet)" in r)
    check(v, f"Alice's edit shown ({v!r})")
    await asyncio.sleep(3)
    await amiga.shot("04e-reply-edit")
    rc, _ = await amiga.rexx("DELETELAST Alice")
    v = await texts(lambda r: "(Nachricht gelöscht)" in r and "Klar doch, gerne" not in r.split("|")[-1])
    check(v, f"our message deleted, shown as deleted ({v!r})")

    # threads: a line under the first message, the thread in its own window
    root = await p.send(dm, "Thread-Start")
    for t in ("im Faden 1", "im Faden 2"):
        await p.c.room_send(dm, "m.room.message", {"msgtype": "m.text", "body": t, "m.relates_to": {
            "rel_type": "m.thread", "event_id": root, "is_falling_back": True,
            "m.in_reply_to": {"event_id": root}}}, ignore_unverified_devices=True)
    v = await texts(lambda r: "Thread-Start" in r and "Thread: 2 Antworten" in r)
    check(v and "im Faden" not in v, f"room window: thread line, not the thread's messages ({v!r})")
    rc, _ = await amiga.rexx("OPENTHREAD Alice")
    check(rc == "0", "thread window opened")

    async def ttexts(want):
        async def f():
            rc, r = await amiga.rexx("TEXTS Alice 20 THREAD")
            return want(r) and r
        return await amiga.until(f, 120)
    v = await ttexts(lambda r: "Thread-Start" in r and "im Faden 1" in r and "im Faden 2" in r)
    check(v, f"thread window: the first message and the thread ({v!r})")
    rc, _ = await amiga.rexx("THREADSAY Alice Ich auch")
    ok = await p.wait(lambda: p.got(dm, me.user_id, "Ich auch"), 120)
    ev = next((e for e in p.events if getattr(e, "body", None) == "Ich auch"), None)
    rel = ((ev.source.get("content") or {}).get("m.relates_to") or {}) if ev else {}
    check(ok and rel.get("rel_type") == "m.thread" and rel.get("event_id") == root, "our message went into the thread")
    v = await texts(lambda r: "Thread: 3 Antworten" in r)
    check(v, f"the room window counts it ({v!r})")
    v = await ttexts(lambda r: "Ich auch" in r)
    check(v, "our thread message shows in the thread window")
    await asyncio.sleep(3)
    await amiga.shot("04f-thread")

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
    rc, _ = await amiga.rexx("PICKER Kaffeeklatsch")
    check(rc == "0", "reaction picker opens (smiley button)")
    await asyncio.sleep(4)
    await amiga.shot("05d-picker")
    rc, _ = await amiga.rexx("REACT Kaffeeklatsch heart")
    v = await reactions(lambda r: "heart 1" in r and "heart 1*" not in r and "heart 2" not in r)
    check(v, f"the same reaction again takes ours back ({v!r})")
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

    # members and room settings (we created Amiga-Runde: we may change everything)
    if new:
        rc, _ = await amiga.rexx("ROOMINFO Amiga-Runde")

        async def members(want="Alice:Mitglied"):
            rc, r = await amiga.rexx("MEMBERS")
            return want in r and ("Gründer" in r or "Admin" in r) and r
        v = await amiga.until(members, 90)
        check(v, f"room window: member list, we found/administer it, Alice a member ({v!r})")
        rc, v = await amiga.rexx("INFOSTATE")
        check(v.startswith("Amiga-Runde|") and "|1|" in v, f"room window: settings shown ({v!r})")
        await amiga.shot("06b-room-info")
        rc, _ = await amiga.rexx("INFOSET topic Rund um den Amiga")
        ok = await p.wait(lambda: p.c.rooms[new].topic == "Rund um den Amiga", 90)
        check(ok, "topic changed in the room window, Alice sees it")
        rc, _ = await amiga.rexx("INFOMEMBER 1 role 1")
        ok = await p.wait(lambda: p.c.rooms[new].power_levels.get_user_level(p.user.user_id) == 50, 90)
        rc2, v = await amiga.rexx("INFOSTATE")
        check(ok, f"Alice made a moderator (rc {rc}, {v!r})")
        v = await amiga.until(lambda: members("Alice:Moderator"), 60)
        check(v, f"the list shows her new role ({v!r})")
        await amiga.rexx("INFOMEMBER 1 role 0")

        # pin, forward, quote (marked message); the edit history of our edited message
        await amiga.rexx("OPENCHAT Amiga-Runde")
        await asyncio.sleep(5)
        rc, _ = await amiga.rexx("MARK Amiga-Runde Danke")
        rc2, _ = await amiga.rexx("ACTION Amiga-Runde pin")

        async def pinbar():
            rc_, r = await amiga.rexx("PINBAR Amiga-Runde")
            return "Danke für die Einladung" in r and r
        v = await amiga.until(pinbar, 90)
        check(v, f"pinned: the bar above the messages shows it (rc {rc}/{rc2}, {v!r})")
        await amiga.shot("06g-pinned")
        await amiga.rexx("ACTION Amiga-Runde forward Alice")
        check(await p.wait_message(dm, me.user_id, "Danke für die Einladung!", 90), "forwarded to Alice's chat")
        await amiga.rexx("MARK Amiga-Runde Danke")
        await amiga.rexx("ACTION Amiga-Runde quote")
        await amiga.rexx("SAY Amiga-Runde Gern geschehen")
        ok = await p.wait(lambda: any(e["content"].get("body", "").startswith("> Danke für die Einladung!")
                                      and "<blockquote>" in e["content"].get("formatted_body", "")
                                      for e in seen_src), 90)
        check(ok, "quoted: the quote above our answer")
    await amiga.rexx("MARK Alice Sehr sch\xf6n")             # Alice's edited answer (ours was deleted)
    await amiga.rexx("ACTION Alice edithist")

    async def hist():
        rc, r = await amiga.rexx("EDITHIST")
        return r.count("|") >= 1 and "Sehr sch\xf6n" in r and r
    v = await amiga.until(hist, 60)
    rc, now = await amiga.rexx("EDITHIST")
    check(v, f"edit history: original and edit ({v!r}; now {now!r})")


    # spaces, the room filter, favourites, message search
    from nio import RoomCreateResponse as RCR
    sp = await p.c.room_create(space=True, name="Hobbys", invite=[me.user_id])
    check(isinstance(sp, RCR), "peer: space created")
    if isinstance(sp, RCR):
        await p.c.room_put_state(sp.room_id, "m.space.child", {"via": [p.user.user_id.split(":", 1)[1]]},
                                 state_key=room)

        async def space_invite():
            rc, r = await amiga.rexx("ACCEPT Hobbys")
            return rc == "0"
        check(await amiga.until(space_invite, 120), "accepted the space invitation")

        async def in_space():
            rc, _ = await amiga.rexx("SPACE Hobbys")
            rc2, r = await amiga.rexx("ROOMS")
            return rc == "0" and r == "Kaffeeklatsch" and r
        v = await amiga.until(in_space, 120)
        check(v, f"space chosen: only its room in the list ({v!r})")
        rc, _ = await amiga.rexx("CHATSTATUS Hobbys")
        check(rc != "0", "no chat window for the space itself")
        await amiga.rexx("SPACEWIN Hobbys")

        async def spacelist():
            rc, r = await amiga.rexx("SPACELIST")
            return "Kaffeeklatsch:joined" in r and r
        v = await amiga.until(spacelist, 60)
        check(v, f"space window: its rooms ({v!r})")
        await amiga.shot("06c-space")
        await amiga.rexx("SPACE")
    rc, v = await amiga.rexx("ROOMS")
    check("Hobbys" not in v and "Alice" in v, f"all rooms again, the space itself not among them ({v!r})")
    await amiga.rexx("FILTER kaffee")
    rc, v = await amiga.rexx("ROOMS")
    check(v == "Kaffeeklatsch", f"filter 'kaffee' ({v!r})")
    await amiga.rexx("FILTER")
    await amiga.rexx("FAVOURITE Amiga-Runde")

    async def fav_first():
        rc, r = await amiga.rexx("ROOMS")
        return r.startswith("Amiga-Runde*") and r
    v = await amiga.until(fav_first, 60)
    check(v, f"favourite: star and on top ({v!r})")
    await amiga.shot("06d-favourite")
    await amiga.rexx("FAVOURITE Amiga-Runde OFF")
    rc, _ = await amiga.rexx("FIND kuchen")

    async def found():
        rc, r = await amiga.rexx("HITS")
        return "Kaffeeklatsch:" in r and "Kuchen" in r and r
    v = await amiga.until(found, 60)
    check(v, f"message search finds 'Kuchen' in the encrypted room (local history) ({v!r})")
    await amiga.shot("06e-search")

    # a replaced room (tombstone): a bar in its window leads to the new room
    old = await p.create_room(invite=[me.user_id], encrypted=False, name="Altversion")

    async def acc_old():
        rc, r = await amiga.rexx("ACCEPT Altversion")
        return rc == "0"
    check(await amiga.until(acc_old, 120), "joined Altversion")
    neu = await p.create_room(invite=[me.user_id], encrypted=False, name="Neuversion")
    await p.c.room_put_state(old, "m.room.tombstone", {"body": "Neuer Raum", "replacement_room": neu})
    await amiga.rexx("OPENCHAT Altversion")

    async def replaced():
        rc, r = await amiga.rexx("REPLACED Altversion")
        return r not in ("", "(no bar)") and r
    v = await amiga.until(replaced, 90)
    check(v, f"the window of the old room shows the bar ({v!r})")
    await amiga.shot("06h-replaced")
    await amiga.rexx("GOREPLACEMENT Altversion")

    async def in_new():
        rc, r = await amiga.rexx("CHATSTATUS Neuversion")
        return rc == "0" and r
    check(await amiga.until(in_new, 120), "a click: joined and opened the new room")
    await amiga.rexx("CLOSECHAT Neuversion")
    await amiga.rexx("CLOSECHAT Altversion")

    # the first unread message: a marker above it when the window opens; a date jump
    await amiga.rexx("CLOSECHAT Kaffeeklatsch")
    await asyncio.sleep(3)
    for t in ("Noch jemand da?", "Hallo?", "Kuchen ist alle."):
        await p.send(room, t)

    async def unread3():
        rc, r = await amiga.rexx("ROOMLINE Kaffeeklatsch")
        mm = re.search(r"\((\d+)", r)
        return mm and int(mm.group(1)) >= 3 and r
    v = await amiga.until(unread3, 90)
    rc, now = await amiga.rexx("ROOMLINE Kaffeeklatsch")
    check(v, f"unread messages in the group ({v!r}; now {now!r})")
    await amiga.rexx("OPENCHAT Kaffeeklatsch")

    async def marker():
        rc, r = await amiga.rexx("VISIBLE Kaffeeklatsch")
        return r and not r.startswith("-1|") and r
    v = await amiga.until(marker, 90)
    check(v, f"marker above the first unread message, in view ({v!r})")
    await amiga.shot("06f-new-marker")
    await amiga.rexx("SAY Kaffeeklatsch /jumptodate 01.01.2020")

    async def jumped():
        rc, r = await amiga.rexx("CHATSTATUS Kaffeeklatsch")
        return "Suche den Tag" not in r and r
    v = await amiga.until(jumped, 120)
    check(v, f"/jumptodate to before the room: loads back to its start ({v!r})")

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
        return n >= 45 and "Nachricht 01" in r and n      # cached messages alone are already 40
    n1 = await amiga.until(lambda: archiv_lines(), 180)
    await asyncio.sleep(20)                  # let live messages and the first history page settle
    n1 = await archiv_lines()
    rc, first1 = await amiga.rexx("FIRSTLINE Archiv")
    check(n1 > 0 and "Nachricht 01" not in first1,
          f"window opens with the newest messages, not all ({n1} lines, top: {first1!r})")
    await amiga.shot("08-archiv-before")
    rc, _ = await amiga.rexx("OLDER Archiv")
    check(rc == "0", "pressed 'Ältere Nachrichten laden'")
    n2 = await amiga.until(has45, 60)
    for _ in range(2):                      # the cache may have shown part of a page: press again
        if n2:
            break
        await amiga.rexx("OLDER Archiv")
        n2 = await amiga.until(has45, 60)
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

    await notify_part(ctx, amiga, p, dm, me)

    await store_part(ctx, amiga, online, bool(info))

    rc, _ = await amiga.rexx("NEWCHAT " + ctx.peer_user.user_id)
    check(rc == "0", "new chat with an existing partner opens the existing chat")
    await asyncio.sleep(5)
    await amiga.shot("07-end")

    # notifications per room on the server, keywords, export, the account password
    await amiga.rexx("NOTIFYLEVEL Kaffeeklatsch 3")

    async def level(n):
        rc, r = await amiga.rexx("NOTIFYLEVEL Kaffeeklatsch")
        rc2, line = await amiga.rexx("ROOMLINE Kaffeeklatsch")
        return r == str(n) and (n != 3 or "stumm" in line) and (r, line)
    v = await amiga.until(lambda: level(3), 60)
    rc, now = await amiga.rexx("NOTIFYLEVEL Kaffeeklatsch")
    rc, now2 = await amiga.rexx("ROOMLINE Kaffeeklatsch")
    check(v, f"group set to off on the server, shown as muted ({v!r}; now {now!r} {now2!r})")
    await amiga.rexx("NOTIFYLEVEL Kaffeeklatsch 0")
    check(await amiga.until(lambda: level(0), 60), "and back to the default")
    await amiga.rexx("KEYWORD Amiga")

    async def kw(want):
        rc, r = await amiga.rexx("KEYWORDS")
        return (r == want) and (r or "-")
    check(await amiga.until(lambda: kw("Amiga"), 60), "keyword added")
    await amiga.rexx("KEYWORD Amiga OFF")
    check(await amiga.until(lambda: kw(""), 60), "keyword removed")
    exp = os.path.join(amiga.extra, "chat.html")
    if os.path.exists(exp):
        os.remove(exp)
    await amiga.rexx("EXPORT Alice Extra:chat.html ALL")

    async def exported():
        if not os.path.exists(exp):
            return False
        t = open(exp, encoding="utf-8", errors="replace").read()
        return "</html>" in t and "Hallo Alice, hier schreibt der Amiga" in t and len(t)
    v = await amiga.until(exported, 120)
    check(v, f"chat exported as HTML with the whole history ({v} bytes)")
    await amiga.rexx(f"PASSWORD {me.password} NeuesPasswort42")

    async def acct(want):
        rc, r = await amiga.rexx("ACCOUNTSTATE")
        return want in r and r
    v = await amiga.until(lambda: acct("Passwort geändert"), 60)
    check(v, f"password changed in the account window ({v!r})")
    await amiga.rexx(f"PASSWORD NeuesPasswort42 {me.password}")
    await amiga.until(lambda: acct("Passwort geändert"), 60)

    # sessions: the list, rename the other one, sign it out (password)
    rc, _ = await amiga.rexx("SESSIONS")

    async def sessions(n=2):
        rc, r = await amiga.rexx("SESSIONLIST")
        parts = [x for x in r.split("|") if x]
        return len(parts) >= n and ":current" in r and parts
    v = await amiga.until(sessions, 60)
    check(v, f"sessions window lists ours and the other ({v!r})")
    if v:
        idx = next(i for i, x in enumerate(v) if not x.endswith(":current"))
        await amiga.rexx(f"SESSIONSET {idx} NAME Testgeraet")

        async def renamed():
            p_ = await sessions()
            return p_ and any(x.startswith("Testgeraet:") for x in p_) and p_
        v2 = await amiga.until(renamed, 60)
        check(v2, f"renamed ({v2!r})")
        await amiga.shot("07b-sessions")
        n0 = len(v)
        idx = next((i for i, x in enumerate(v2 or []) if x.startswith("Testgeraet:")), None)
        if idx is not None:
            await amiga.rexx(f"SESSIONSET {idx} PASSWORD {me.password}")

        async def fewer():
            rc, r = await amiga.rexx("SESSIONLIST")
            parts = [x for x in r.split("|") if x]
            return parts and len(parts) < n0 and "Testgeraet" not in r and parts
        v3 = await amiga.until(fewer, 60)
        check(v3, f"the other session signed out ({v3!r})")

    # own profile: name and picture
    from nio import ProfileGetResponse
    rc, _ = await amiga.rexx("PROFILE NAME Amiga-Fan")

    async def prof(cond):
        r = await p.c.get_profile(me.user_id)
        return isinstance(r, ProfileGetResponse) and cond(r)
    check(await amiga.until(lambda: prof(lambda r: r.displayname == "Amiga-Fan"), 90),
          "profile window: new display name, Alice sees it")
    open(os.path.join(amiga.extra, "ich.png"), "wb").write(png(64, 64, (200, 40, 40)))
    rc, _ = await amiga.rexx("PROFILE PICTURE Extra:ich.png")
    check(await amiga.until(lambda: prof(lambda r: str(r.avatar_url).startswith("mxc://")), 90),
          "profile picture uploaded and set")

    async def shown():
        rc, r = await amiga.rexx("PROFILESTATE")
        return r.startswith("Amiga-Fan|1|") and r
    v = await amiga.until(shown, 60)
    check(v, f"profile window shows name and picture ({v!r})")
    await amiga.shot("07c-profile")

    # leave the room created in the GUI for good: window closes, room gone
    if new:
        rc, _ = await amiga.rexx("LEAVE Amiga-Runde")
        check(rc == "0", "left Amiga-Runde (toolbar door / menu, without the question)")
        check(await p.wait_left(new, me.user_id, 120), "peer sees us leave")

        async def gone():
            rc, r = await amiga.rexx("ROOMLINE Amiga-Runde")
            return rc != "0"
        check(await amiga.until(gone, 60), "Amiga-Runde is no longer in the room list")


async def herald(amiga, query):
    """Asks Herald (in the emulator) something; '' if it is not running."""
    res = os.path.join(amiga.extra, "res.txt")
    if os.path.exists(res):
        os.remove(res)
    await amiga.rexx_raw(["options results", "if ~show('P', 'HERALD') then exit",
                          "address HERALD", f"'{query}'", "say 'R=' || result"])
    out = open(res, encoding="latin-1").read() if os.path.exists(res) else ""
    return next((l[2:].strip() for l in out.splitlines() if l.startswith("R=")), "")


async def notify_part(ctx, amiga, p, dm, me):
    check = ctx.check
    if not await herald(amiga, "HERALD_QUERY VERSION"):
        check(True, "# Herald not in the emulator: notifications not tested")
        return
    unread0 = await herald(amiga, "HERALD_QUERY UNREAD")
    rc, _ = await amiga.rexx("HIDE")                 # MUI: iconify (the AppIcon)
    check(rc == "0", "iconified")
    await p.send(dm, "Bist du noch da?")

    async def notified():
        apps = await herald(amiga, "HERALD_QUERY APPS")
        n = await herald(amiga, "HERALD_QUERY UNREAD")
        return "Amatrix" in apps and n.isdigit() and int(n) > int(unread0 or 0) and n
    v = await amiga.until(notified, 90)
    check(v, f"Herald shows a notification for the new message (unread {unread0} -> {v})")
    await asyncio.sleep(2)
    await amiga.shot("07-notification")
    rc, _ = await amiga.rexx("MUTE Alice")
    n1 = await herald(amiga, "HERALD_QUERY UNREAD")
    await p.send(dm, "Stumm?")
    await asyncio.sleep(15)
    n2 = await herald(amiga, "HERALD_QUERY UNREAD")
    check(n1 == n2, f"a muted room gives no notification ({n1} -> {n2})")
    await amiga.rexx("MUTE Alice OFF")
    rc, _ = await amiga.rexx(f"OPENCHAT {dm}")       # what Herald sends on a double-click
    check(rc == "0", "the notification's back-message opens the chat (and uniconifies)")
    await asyncio.sleep(2)


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
        mk = os.path.join(TOP, "tools", "amiga-package", "make-icon.py")   # icons: AppIcon when iconified
        os.makedirs(os.path.join(extra, "Icons"), exist_ok=True)
        subprocess.run([sys.executable, mk, os.path.join(extra, "Amatrix.info"), "65536"], check=False)
        subprocess.run([sys.executable, mk, os.path.join(extra, "Icons", "Amatrix-neu.info"), "65536", "new"],
                       check=False)
        hsrc = os.path.join(TOP, "third_party", "herald", "Herald", "Files")
        if not os.path.isdir(hsrc):
            subprocess.run([os.path.join(TOP, "third_party", "fetch-herald.sh")], check=False)
        herald = ""
        if os.path.isdir(hsrc):                       # notifications: Herald in the emulator
            shutil.copytree(hsrc, os.path.join(extra, "Herald"))
            herald = "Run >NIL: Extra:Herald/Herald\nWait 3"
        script = os.path.join(extra, ".autotest")
        v = os.environ.get("AMX_VERBOSE")
        with open(script, "w", encoding="latin-1") as f:
            f.write(LOOP.format(server=srv.https_url, user=me.localpart, password=me.password, herald=herald,
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
        dbg = os.path.join(extra, "Amatrix.log")
        if os.path.exists(dbg):              # the interesting lines of the DEBUG log
            for line in open(dbg, encoding="latin-1").read().splitlines():
                if any(k in line for k in ("open file", "open link", "saved attachment", "started", "moderate",
                                           "room change", "INFOMEMBER", "sending file", "lookup event", "close_chat", "cursor up", "start_edit", "ARexx", "edit button", "older pressed",
                                           "history_done", "display option", "place hist",
                                           "decode")):
                    log(f"# amatrix log: {line}")
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
