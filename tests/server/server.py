"""A private, throw-away Matrix homeserver for the Amatrix tests.

Starts Synapse (same version as the server the real tests run against) in a
fresh temporary directory with its own config, plus a TLS front with a
private test CA, so the tests do not depend on any existing server.

    with TestServer() as srv:
        alice = srv.register("alice")      # -> TestUser(user_id, password, localpart)
        srv.http_url      # http://127.0.0.1:<port>
        srv.https_url     # https://localhost:<port>  (cert from srv.ca_pem)

The Python environment (Synapse + matrix-nio for the peers) is created on
first use in $AMX_VENV (default ~/.cache/amatrix/venv); see ensure_venv().
"""
import hashlib, hmac, json, os, shutil, socket, subprocess, sys, tempfile, time, urllib.request
from dataclasses import dataclass

SYNAPSE_VERSION = "1.164.1"
HERE = os.path.dirname(os.path.abspath(__file__))
VENV = os.environ.get("AMX_VENV", os.path.expanduser("~/.cache/amatrix/venv"))
SERVER_NAME = "amatrix.test"


def venv_python():
    return os.path.join(VENV, "bin", "python")


def ensure_venv():
    """Create the venv with Synapse and matrix-nio[e2e] if it is missing."""
    py = venv_python()
    if os.path.exists(py):
        r = subprocess.run([py, "-c", "import synapse, nio, olm"], capture_output=True)
        if r.returncode == 0:
            return py
    print(f"[server] creating Python environment in {VENV} (once, takes a few minutes)", flush=True)
    subprocess.run([sys.executable, "-m", "venv", VENV], check=True)
    subprocess.run([py, "-m", "pip", "install", "-q", f"matrix-synapse=={SYNAPSE_VERSION}",
                    "matrix-nio[e2e]"], check=True)
    return py


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


@dataclass
class TestUser:
    localpart: str
    user_id: str
    password: str


class TestServer:
    def __init__(self, keep=False, workdir=None):
        self.keep = keep
        self.dir = workdir or tempfile.mkdtemp(prefix="amatrix-synapse-")
        self.port = free_port()
        self.tls_port = free_port()
        self.bad_tls_port = free_port()
        self.secret = os.urandom(16).hex()
        self.http_url = f"http://127.0.0.1:{self.port}"
        self.https_url = f"https://localhost:{self.tls_port}"
        # same server, but a self-signed certificate nobody trusts (TLS negative tests)
        self.untrusted_url = f"https://localhost:{self.bad_tls_port}"
        self.ca_pem = os.path.join(self.dir, "tls", "ca.pem")
        self.procs = []
        self._n = 0

    # ------------------------------------------------------------ lifecycle
    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *a):
        self.stop()

    def start(self):
        py = ensure_venv()
        cfg = os.path.join(self.dir, "homeserver.yaml")
        subprocess.run([py, "-m", "synapse.app.homeserver", "--server-name", SERVER_NAME,
                        "--config-path", cfg, "--generate-config", "--report-stats=no",
                        "--data-directory", self.dir], check=True, capture_output=True)
        y = open(cfg).read()
        y = y.replace("port: 8008", f"port: {self.port}")
        y = y.replace("    - ::1\n", "")                       # no IPv6 needed
        y = y.replace(f'log_config: "{self.dir}/{SERVER_NAME}.log.config"', "")
        y += f"""
# --- Amatrix test server ---
enable_registration: false
registration_shared_secret: "{self.secret}"
suppress_key_server_warning: true
trusted_key_servers: []
report_stats: false
presence:
  enabled: false
# newer Synapse lets only admins publish rooms to the directory by default
room_list_publication_rules:
  - action: allow
"""
        for rc in ["rc_message", "rc_registration", "rc_key_requests", "rc_3pid_validation",
                   "rc_admin_redaction", "rc_media_create", "rc_delayed_event_mgmt"]:
            y += f"{rc}: {{per_second: 10000, burst_count: 10000}}\n"
        y += "rc_login:\n" + "".join(f"  {k}: {{per_second: 10000, burst_count: 10000}}\n"
                                    for k in ["address", "account", "failed_attempts"])
        y += "rc_joins:\n  local: {per_second: 10000, burst_count: 10000}\n" \
             "  remote: {per_second: 10000, burst_count: 10000}\n"
        y += "rc_invites:\n" + "".join(f"  {k}: {{per_second: 10000, burst_count: 10000}}\n"
                                      for k in ["per_room", "per_user", "per_issuer"])
        open(cfg, "w").write(y)
        log = open(os.path.join(self.dir, "synapse.log"), "w")
        self.procs.append(subprocess.Popen([py, "-m", "synapse.app.homeserver", "-c", cfg],
                                           cwd=self.dir, stdout=log, stderr=subprocess.STDOUT))
        self._wait(f"{self.http_url}/_matrix/client/versions", 90, "Synapse")
        self._start_tls(py)
        print(f"[server] Synapse {SYNAPSE_VERSION} on {self.http_url} and {self.https_url} "
              f"(data {self.dir})", flush=True)

    def _start_tls(self, py):
        d = os.path.join(self.dir, "tls")
        os.makedirs(d, exist_ok=True)
        q = dict(cwd=d, check=True, capture_output=True)
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key",
                        "-out", "ca.pem", "-days", "30", "-subj", "/CN=Amatrix Test CA",
                        "-addext", "basicConstraints=critical,CA:TRUE",
                        "-addext", "keyUsage=critical,keyCertSign,cRLSign"], **q)
        subprocess.run(["openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", "srv.key",
                        "-out", "srv.csr", "-subj", "/CN=localhost"], **q)
        open(os.path.join(d, "ext.cnf"), "w").write(
            "subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\n"
            "keyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
        subprocess.run(["openssl", "x509", "-req", "-in", "srv.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
                        "-CAcreateserial", "-out", "srv.pem", "-days", "30", "-extfile", "ext.cnf"], **q)
        log = open(os.path.join(self.dir, "tlsproxy.log"), "w")
        self.procs.append(subprocess.Popen(
            [py, os.path.join(HERE, "tlsproxy.py"), os.path.join(d, "srv.pem"), os.path.join(d, "srv.key"),
             str(self.tls_port), str(self.port)], stdout=log, stderr=subprocess.STDOUT))
        self._wait_tcp(self.tls_port, 20, "TLS proxy")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "bad.key",
                        "-out", "bad.pem", "-days", "30", "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"], **q)
        self.procs.append(subprocess.Popen(
            [py, os.path.join(HERE, "tlsproxy.py"), os.path.join(d, "bad.pem"), os.path.join(d, "bad.key"),
             str(self.bad_tls_port), str(self.port)], stdout=log, stderr=subprocess.STDOUT))
        self._wait_tcp(self.bad_tls_port, 20, "untrusted TLS proxy")

    def ca_hash_name(self):
        """OpenSSL hash file name (e.g. 1d4bd0f8.0) for installing the CA in AmiSSL:Certs."""
        h = subprocess.run(["openssl", "x509", "-hash", "-noout", "-in", self.ca_pem],
                           capture_output=True, text=True, check=True).stdout.strip()
        return h + ".0"

    def stop(self):
        for p in reversed(self.procs):
            p.terminate()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        self.procs = []
        if not self.keep:
            shutil.rmtree(self.dir, ignore_errors=True)

    def _wait(self, url, secs, what):
        end = time.time() + secs
        while time.time() < end:
            if any(p.poll() is not None for p in self.procs):
                raise RuntimeError(f"{what} exited, see {self.dir}")
            try:
                urllib.request.urlopen(url, timeout=2)
                return
            except Exception:
                time.sleep(0.5)
        raise RuntimeError(f"{what} did not start within {secs}s, see {self.dir}")

    def _wait_tcp(self, port, secs, what):
        end = time.time() + secs
        while time.time() < end:
            try:
                socket.create_connection(("127.0.0.1", port), 1).close()
                return
            except OSError:
                time.sleep(0.3)
        raise RuntimeError(f"{what} did not start")

    # ---------------------------------------------------------------- users
    def register(self, prefix, admin=False):
        """Registers a fresh user (unique per call): <prefix><n>, password <localpart>-pw."""
        self._n += 1
        localpart = f"{prefix}{self._n}"
        password = f"{localpart}-pw"
        url = f"{self.http_url}/_synapse/admin/v1/register"
        nonce = json.load(urllib.request.urlopen(url))["nonce"]
        mac = hmac.new(self.secret.encode(), digestmod=hashlib.sha1)
        mac.update(b"\x00".join([nonce.encode(), localpart.encode(), password.encode(),
                                 b"admin" if admin else b"notadmin"]))
        body = json.dumps({"nonce": nonce, "username": localpart, "password": password,
                           "admin": admin, "mac": mac.hexdigest()}).encode()
        req = urllib.request.Request(url, body, {"Content-Type": "application/json"})
        r = json.load(urllib.request.urlopen(req))
        return TestUser(localpart, r["user_id"], password)
