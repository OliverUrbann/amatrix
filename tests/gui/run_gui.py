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
    await amiga.shot("04-chat-reply")

    rc, _ = await amiga.rexx("OPENCHAT Kaffeeklatsch")
    check(rc == "0", "opened the group room")
    await asyncio.sleep(10)
    await amiga.shot("05-group")

    rc, _ = await amiga.rexx(f"NEWROOM Amiga-Runde {ctx.peer_user.user_id}")
    check(rc == "0", "new room command")
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

    rc, _ = await amiga.rexx("NEWCHAT " + ctx.peer_user.user_id)
    check(rc == "0", "new chat with an existing partner opens the existing chat")
    await asyncio.sleep(5)
    await amiga.shot("07-end")


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
        me, other = srv.register("amiga"), srv.register("peer")
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
            os.path.join(TOP, "tests", "emu", "run-amiga-gui.sh"), script, "1500", extra, SHOTS,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)

        from peerlib import Context
        workdir = tempfile.mkdtemp(prefix="amx-gui-peer-")
        ctx = Context(srv.http_url, other, me, workdir, log)
        ctx.scale = 3
        amiga = Amiga(extra, log)
        t0 = time.time()
        try:
            await asyncio.wait_for(scenario(ctx, amiga, srv, me), 1300)
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
