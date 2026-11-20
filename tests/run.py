#!/usr/bin/env python3
"""Runs the Amatrix feature tests against a private, freshly started Synapse.

    tests/run.py [--target native|asan|amiga] [--keep] [-v] [feature ...]

For every feature in tests/features/<name>/ (test.c + peer.py):
  - two fresh users are registered (client under test, peer),
  - the C test runs (on Linux, or in FS-UAE for --target amiga) while the
    peer (matrix-nio, same crypto as Element) acts from the host,
  - both report TAP; the feature passes if both pass.

Needs: openssl, a C toolchain (and /opt/amiga + the FS-UAE test environment
with AmiSSL for --target amiga, see tests/emu/README.md).  The Python
environment for Synapse/nio is created on first use (tests/server/server.py).
Logs: build/<target>/test-logs/<feature>.log
"""
import argparse, asyncio, importlib.util, os, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, "server"))
sys.path.insert(0, HERE)
from server import TestServer, venv_python, ensure_venv   # noqa: E402

TESTENV = os.environ.get("TESTENV", os.path.expanduser("~/amiga/testenv/amiga-testenv"))


def features():
    d = os.path.join(HERE, "features")
    return sorted(f for f in os.listdir(d) if os.path.exists(os.path.join(d, f, "test.c")))


def load_peer(feature):
    path = os.path.join(HERE, "features", feature, "peer.py")
    spec = importlib.util.spec_from_file_location(f"peer_{feature}", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def parse_tap(text):
    oks = [l for l in text.splitlines() if l.startswith("ok ")]
    bad = [l for l in text.splitlines() if l.startswith("not ok ")]
    plan = any(l.startswith("1..") for l in text.splitlines())
    return len(oks), bad, plan


# ------------------------------------------------------------ test runners

async def run_native(binary, args, env, timeout, log):
    p = await asyncio.create_subprocess_exec(binary, *args, env=env, stdout=asyncio.subprocess.PIPE,
                                             stderr=asyncio.subprocess.STDOUT)
    out = []

    async def reader():
        async for raw in p.stdout:
            line = raw.decode("utf-8", "replace").rstrip("\n")
            out.append(line)
            log(line)
    try:
        await asyncio.wait_for(asyncio.gather(reader(), p.wait()), timeout)
    except asyncio.TimeoutError:
        p.kill()
        out.append("not ok - test program timed out")
        log(out[-1])
    return "\n".join(out), p.returncode


async def run_amiga(binary, args, srv, timeout, log):
    sysdir = os.path.join(TESTENV, "system")
    if not os.path.isdir(os.path.join(sysdir, "AmiSSL")):
        raise RuntimeError(f"AmiSSL missing in {sysdir}: run tests/emu/setup-emu-amissl.sh")
    shutil.copy(srv.ca_pem, os.path.join(sysdir, "AmiSSL", "Certs", srv.ca_hash_name()))
    extra = tempfile.mkdtemp(prefix="amx-emu-")
    shutil.copy(binary, os.path.join(extra, "test"))
    q = lambda a: '"' + a.replace('"', "") + '"'
    script = os.path.join(extra, ".autotest")
    with open(script, "w", encoding="latin-1") as f:
        verbose = os.environ.get("AMX_VERBOSE")
        f.write(f"SetEnv AMX_VERBOSE {verbose}\n" if verbose else "UnSetEnv AMX_VERBOSE\n")
        f.write("Stack 262144\nAssign AmiSSL: SYS:AmiSSL\nAssign LIBS: AmiSSL:Libs ADD\n"
                "Echo \"# emulator: FS-UAE\" >SYS:out.txt\n"
                f"Extra:test {' '.join(q(a) for a in args)} >>SYS:out.txt\n"
                "Echo \"# rc=$RC\" >>SYS:out.txt\nEcho \"done\" >SYS:amiga-done\n")
    out_file = os.path.join(sysdir, "out.txt")
    if os.path.exists(out_file):
        os.remove(out_file)
    p = await asyncio.create_subprocess_exec(os.path.join(TESTENV, "run-amiga.sh"), script,
                                             str(int(timeout)), extra,
                                             stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    await p.communicate()
    text = open(out_file, encoding="latin-1").read() if os.path.exists(out_file) else "not ok - no output"
    for line in text.splitlines():
        log(line)
    shutil.rmtree(extra, ignore_errors=True)
    rc = 0
    for line in text.splitlines():
        if line.startswith("# rc="):
            rc = int(line[5:] or 0)
    return text, rc


REMOTE = {}          # target -> (Remote, VM): the emulated OS 4 / MorphOS, up for the whole run


async def run_remote(target, binary, args, srv, timeout, log):
    remote = REMOTE[target][0]
    q = lambda a: '"' + a.replace('"', "") + '"'
    with open(binary, "rb") as f:
        exe = f.read()
    with open(srv.ca_pem, "rb") as f:
        ca = f.read()
    vm = REMOTE[target][1]
    await remote.run("Delete RAM:amx-store ALL QUIET FORCE", timeout=60)
    text = await remote.run(f"RAM:test {' '.join(q(a) for a in args)}",
                            files={"RAM:test": exe, vm.cert_path or f"AmiSSL:Certs/{srv.ca_hash_name()}": ca},
                            timeout=timeout)
    if "timed out" in text and "# rc=" not in text:      # hangs or crashed: what does the system say?
        shot = os.path.join(TOP, "build", target, "test-logs", f"{os.path.basename(binary)}.png")
        vm.screenshot(shot)
        dbg = os.path.join(TOP, "build", target, "test-logs", f"{os.path.basename(binary)}.debug.txt")
        with open(dbg, "w", encoding="latin-1") as f:
            for cmd in vm.diag:
                f.write(await remote.sh(cmd))
            if hasattr(vm, "serial_log"):
                f.write("\n--- serial ---\n" + vm.serial_log())
        part = await remote.sh("Type RAM:amxrun.out")          # how far the program got
        tasks = await remote.sh("Status FULL")
        with open(dbg, "a", encoding="latin-1") as f:
            f.write("\n--- Status FULL ---\n" + tasks)
        if part == "(no answer)":                           # amxrun itself is unreachable
            text += "# emulator network lost\n"
        text += "# output so far:\n" + "".join("#   " + l + "\n" for l in part.splitlines())
        text += f"\n# screenshot {shot}\n# debug buffer {dbg}\n"
        # a crashed program leaves the system stuck (Grim Reaper): start it afresh
        remote.close()
        await asyncio.get_running_loop().run_in_executor(None, vm.stop)
        REMOTE.pop(target)
        await start_remote(target)
    text = f"# emulator: {target}\n" + text
    for line in text.splitlines():
        log(line)
    rc = 0
    for line in text.splitlines():
        if line.startswith("# rc="):
            rc = int(line[5:] or 0)
    return text, rc


async def start_remote(target):
    sys.path.insert(0, os.path.join(HERE, "emu"))
    from remote import Remote, Os4Vm, MosVm
    remote = Remote()
    vm = (MosVm if target == "mos" else Os4Vm)(remote.port)
    print(f"starting the emulated {target} ...", flush=True)
    await asyncio.get_running_loop().run_in_executor(None, vm.start)
    if not await remote.wait_ready(400):
        vm.screenshot(os.path.join(TOP, "build", target, "emu-start.png"))
        vm.stop()
        raise RuntimeError(f"amxrun in the emulated {target} did not report (screenshot build/{target}/emu-start.png)")
    REMOTE[target] = (remote, vm)


def stop_remote():
    for remote, vm in REMOTE.values():
        remote.quit()
        time.sleep(3)
        vm.stop()
        remote.close()
    REMOTE.clear()


# -------------------------------------------------------------------- main

async def run_feature(feature, target, srv, logdir, verbose):
    logf = open(os.path.join(logdir, f"{feature}.log"), "w", encoding="utf-8")

    def log(line):
        logf.write(line + "\n")
        logf.flush()
        if verbose:
            print(f"  [{feature}] {line}", flush=True)

    mod = load_peer(feature)
    emulated = target in ("amiga", "os4", "mos")
    scale = int(os.environ.get("AMX_SCALE", 3)) if emulated else 1     # whole number: peers use it in range()
    timeout = getattr(mod, "TIMEOUT", 240) * scale
    me, other = srv.register("amiga"), srv.register("peer")
    workdir = tempfile.mkdtemp(prefix=f"amx-{feature}-")
    from peerlib import Context
    ctx = Context(srv.http_url, other, me, workdir, log)
    ctx.scale = scale
    ctx.server = srv
    https, untrusted = srv.https_url, srv.untrusted_url
    if target in REMOTE:                          # the host as the emulated system sees it
        https, untrusted = (u.replace("localhost", "10.0.2.2") for u in (https, untrusted))
    extra = [a.format(untrusted=untrusted) for a in getattr(mod, "EXTRA_ARGS", [])]
    binary = os.path.join(TOP, "build", target, "tests", feature)
    store = "RAM:amx-store" if emulated else os.path.join(workdir, "store")
    args = [https, me.localpart, me.password, other.user_id, store] + extra
    log(f"# {feature}: client {me.user_id}, peer {other.user_id}, target {target}")

    if target == "amiga":
        test = asyncio.ensure_future(run_amiga(binary, args, srv, timeout, log))
    elif target in REMOTE:
        test = asyncio.ensure_future(run_remote(target, binary, args, srv, timeout, log))
    else:
        env = dict(os.environ, SSL_CERT_FILE=srv.ca_pem, ASAN_OPTIONS="detect_leaks=0",
                   UBSAN_OPTIONS="print_stacktrace=0")
        test = asyncio.ensure_future(run_native(binary, args, env, timeout, log))
    try:
        await asyncio.wait_for(mod.peer(ctx), timeout)
    except asyncio.TimeoutError:
        ctx.check(False, "peer finished in time")
    except Exception as e:
        ctx.check(False, f"peer raised {type(e).__name__}: {e}")
    finally:
        await ctx.close()
    text, rc = await test
    n_ok, bad, plan = parse_tap(text)
    peer_bad = [n for ok, n in ctx.results if not ok]
    sanitizer = "ERROR: AddressSanitizer" in text
    passed = rc == 0 and plan and not bad and not peer_bad and n_ok > 0 and not sanitizer
    logf.close()
    shutil.rmtree(workdir, ignore_errors=True)
    detail = []
    if not plan: detail.append("no TAP plan (crash?)")
    if rc: detail.append(f"rc={rc}")
    if sanitizer: detail.append("sanitizer error")
    if "# emulator network lost" in text: detail.append("emulator network lost")
    detail += [b for b in bad] + [f"peer: {n}" for n in peer_bad]
    return passed, n_ok + sum(ok for ok, _ in ctx.results), detail


async def main():
    import logging
    logging.basicConfig(level=logging.ERROR)      # nio is chatty on stdout
    for n in ("nio", "peewee", "asyncio"):
        logging.getLogger(n).setLevel(logging.ERROR)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", default="native", choices=["native", "asan", "amiga", "os4", "mos"])
    ap.add_argument("--keep", action="store_true", help="keep the server data directory")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("feature", nargs="*")
    a = ap.parse_args()
    feats = a.feature or features()

    print(f"building ({a.target}) ...", flush=True)
    goals = ["tests"] + ([f"build/{a.target}/tools/amxrun"] if a.target in ("os4", "mos") else [])
    r = subprocess.run(["make", "-j2", f"TARGET={a.target}"] + goals, cwd=TOP,
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:])
        return 2
    ensure_venv()
    logdir = os.path.join(TOP, "build", a.target, "test-logs")
    os.makedirs(logdir, exist_ok=True)
    results = []
    if a.target in ("os4", "mos"):
        await start_remote(a.target)
    try:
        with TestServer(keep=a.keep) as srv:
            for f in feats:
                t0 = time.time()
                print(f"{f:28s} ...", end="", flush=True)
                ok, n, detail = await run_feature(f, a.target, srv, logdir, a.verbose)
                if not ok and "emulator network lost" in detail:   # the emulator, not us: once more
                    os.replace(os.path.join(logdir, f"{f}.log"), os.path.join(logdir, f"{f}.lost.log"))
                    print(f"\r{f:28s} ...emulator network lost, again ...", end="", flush=True)
                    ok, n, detail = await run_feature(f, a.target, srv, logdir, a.verbose)
                print(f"\r{f:28s} {'PASS' if ok else 'FAIL'}  ({n} checks, {time.time() - t0:.0f}s)", flush=True)
                for d in detail:
                    print(f"    {d}")
                results.append(ok)
    finally:
        stop_remote()
    print(f"\n{sum(results)}/{len(results)} features passed on {a.target}; logs in {logdir}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    # the peers need matrix-nio: re-run inside the test venv if necessary
    try:
        import nio  # noqa: F401
    except ImportError:
        py = ensure_venv()
        os.execv(py, [py] + sys.argv)
    sys.exit(asyncio.run(main()))
