"""Running test programs inside an emulated AmigaOS 4 (or MorphOS) that stays
up for the whole test run.

The emulated system starts amxrun (tests/emu/amxrun.c), which fetches jobs
from a small HTTP server here over the emulator's network (SLIRP: the host is
10.0.2.2 for the guest, and 10.0.2.2 reaches 127.0.0.1 on the host):

    remote = Remote()                 # job server on 127.0.0.1:<port>
    vm = Os4Vm(remote.port); vm.start()
    text = await remote.run("RAM:test args", files={"RAM:test": data}, timeout=600)
    vm.stop()

Environments (from the Nextcloud claude-env): OS4ENV = Emulation/AmigaOS4-Linux
(os41fe.hdf, os4.fs-uae, set up with setup-os4-linux.sh), TESTENV = the OS 3
test environment (used to copy amxrun onto the OS 4 disk before booting).
"""
import asyncio, os, zlib, shutil, subprocess, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
GUEST_HOST = "10.0.2.2"          # the host as the emulated system sees it
OS4ENV = os.environ.get("OS4ENV", "/root/env/nc/claude-env/Emulation/AmigaOS4-Linux")
TESTENV = os.environ.get("TESTENV", os.path.expanduser("~/amiga/testenv/amiga-testenv"))


class Remote:
    """Job queue for amxrun: one job at a time, results come back as text."""

    def __init__(self):
        self.jobs = []                      # bytes, served in order
        self.lock = threading.Lock()
        self.result = None
        self.result_ev = threading.Event()
        self.sh_result = None
        self.sh_ev = threading.Event()
        self.last_poll = 0.0
        self.files = {}                     # path -> data, to send again
        self.bad = 0                        # files that arrived corrupted
        remote = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                remote.last_poll = time.time()
                with remote.lock:
                    body = remote.jobs.pop(0) if remote.jobs else b"WAIT\n"
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                data = self.rfile.read(n)
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()
                if self.path == "/bad":     # corrupted on the way: once more, first
                    path = data.decode("latin-1")
                    remote.bad += 1
                    print(f"[remote] {path} arrived corrupted, sending it again", flush=True)
                    if path in remote.files:
                        remote.put(path, remote.files[path], first=True)
                elif self.path == "/sh":
                    remote.sh_result = data.decode("latin-1")
                    remote.sh_ev.set()
                else:
                    remote.result = data.decode("latin-1")
                    remote.result_ev.set()

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def put(self, path, data, first=False):
        job = f"PUT {path} {len(data)} {zlib.crc32(data):08x}\n".encode() + data
        with self.lock:
            self.files[path] = data
            self.jobs.insert(0, job) if first else self.jobs.append(job)

    async def wait_ready(self, timeout):
        """True once amxrun has asked for work."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.last_poll:
                return True
            await asyncio.sleep(2)
        return False

    async def run(self, cmd, files=None, timeout=600):
        """Puts the files, runs cmd; returns its output (ends with '# rc=N')."""
        for path, data in (files or {}).items():
            self.put(path, data)
        self.result_ev.clear()
        with self.lock:
            self.jobs.append(f"RUN {cmd}\n".encode("latin-1"))
        ok = await asyncio.get_running_loop().run_in_executor(None, self.result_ev.wait, timeout)
        return self.result if ok else "not ok - test program timed out (no result from the emulator)"

    async def sh(self, cmd, timeout=60):
        """A short command in the emulated system (also while a test runs or hangs)."""
        self.sh_ev.clear()
        with self.lock:
            self.jobs.insert(0, f"SH {cmd}\n".encode("latin-1"))
        ok = await asyncio.get_running_loop().run_in_executor(None, self.sh_ev.wait, timeout)
        return self.sh_result if ok else "(no answer)"

    def quit(self):
        with self.lock:
            self.jobs.append(b"QUIT\n")

    def close(self):
        self.httpd.shutdown()


class Os4Vm:
    """AmigaOS 4.1 FE in FS-UAE + QEMU-UAE (headless), network over SLIRP."""

    def __init__(self, port, amxrun=None, display=":9"):
        self.port = port
        self.amxrun = amxrun or os.path.join(TOP, "build", "os4", "tools", "amxrun")
        self.display = display
        self.procs = []

    def start(self):
        e = tempfile.mkdtemp(prefix="amx-os4-")
        os.mkdir(os.path.join(e, "bin"))
        shutil.copy(self.amxrun, os.path.join(e, "bin", "amxrun"))
        with open(os.path.join(e, "autorun"), "w") as f:
            f.write("AddNetInterface ETHERNET >NIL:\nWait 10\n"
                    f"Run >NIL: <NIL: SYS:claude-test/bin/amxrun {GUEST_HOST} {self.port}\n")
        with open(os.path.join(e, "prep"), "w") as f:      # in OS 3, with the OS 4 disk mounted
            f.write("FailAt 1000\nMakedir AmigaOS4.1FE:claude-test\n"
                    "Delete AmigaOS4.1FE:claude-test/bin ALL QUIET\n"
                    "Copy Extra:bin AmigaOS4.1FE:claude-test/bin ALL CLONE\n"
                    "Copy Extra:autorun AmigaOS4.1FE:claude-test/autorun\n"
                    "Search AmigaOS4.1FE:S/user-startup \"Claude-Testhook\" QUIET\n"
                    "If WARN\n"
                    "  Echo \"\" >>AmigaOS4.1FE:S/user-startup\n"
                    "  Echo \"; Claude-Testhook\" >>AmigaOS4.1FE:S/user-startup\n"
                    "  Echo \"If EXISTS SYS:claude-test/autorun\" >>AmigaOS4.1FE:S/user-startup\n"
                    "  Echo \"  Execute SYS:claude-test/autorun\" >>AmigaOS4.1FE:S/user-startup\n"
                    "  Echo \"EndIf\" >>AmigaOS4.1FE:S/user-startup\n"
                    "EndIf\nEcho \"done\" >SYS:amiga-done\n")
        shutil.copy(os.path.join(OS4ENV, "run-os3-hdf.sh"), os.path.join(TESTENV, "run-hdf.sh"))
        subprocess.run([os.path.join(TESTENV, "run-hdf.sh"), os.path.join(e, "prep"), "150",
                        os.path.join(OS4ENV, "os41fe.hdf"), e], capture_output=True)
        shutil.rmtree(e, ignore_errors=True)
        self.procs.append(subprocess.Popen(["Xvfb", self.display, "-screen", "0", "1024x768x24"],
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(2)
        self.procs.append(subprocess.Popen(["fs-uae", os.path.join(OS4ENV, "os4.fs-uae")], cwd=OS4ENV,
                                           env=dict(os.environ, DISPLAY=self.display),
                                           stdout=open(os.path.join(OS4ENV, "fsuae.out"), "w"),
                                           stderr=subprocess.STDOUT))

    def screenshot(self, path):
        subprocess.run(f"DISPLAY={self.display} import -window root {path}", shell=True)

    def stop(self):
        for p in reversed(self.procs):
            p.terminate()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        self.procs = []
