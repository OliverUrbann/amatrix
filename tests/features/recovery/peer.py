"""recovery: see test.c.

The peer plays Element on a second device of the *same* user: it sets up
cross-signing, a key backup and secret storage (the way Element does it), and
writes messages that the client's device never gets keys for - only the
backup has them.  Implemented independently of the C code: python-olm for
signing and backup encryption, `cryptography` for X25519/HKDF/AES/HMAC.
"""
import asyncio, base64, hashlib, hmac, json, os, time, urllib.parse, urllib.request, urllib.error

TIMEOUT = 420
PASSPHRASE = "Amiga rockt seit 1985"
SALT = "amatrix-test-salt"
ITERATIONS = 20000          # Element uses 500000; kept small for the emulated 68030
MESSAGES = [f"aus der Zeit vor Amatrix {i}" for i in range(1, 6)]

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58encode(b):
    n = int.from_bytes(b, "big")
    out = ""
    while n:
        n, r = divmod(n, 58)
        out = B58[r] + out
    return "1" * (len(b) - len(b.lstrip(b"\0"))) + out


def recovery_key(key):
    raw = bytes([0x8B, 0x01]) + key
    parity = 0
    for x in raw:
        parity ^= x
    s = b58encode(raw + bytes([parity]))
    return " ".join(s[i:i + 4] for i in range(0, len(s), 4))


SSSS_KEY = hashlib.pbkdf2_hmac("sha512", PASSPHRASE.encode(), SALT.encode(), ITERATIONS, 32)
RECOVERY_KEY = recovery_key(SSSS_KEY)
EXTRA_ARGS = [RECOVERY_KEY, PASSPHRASE]


def b64(b):
    return base64.b64encode(b).decode()            # Element: padded base64


def b64u(b):
    return base64.b64encode(b).decode().rstrip("=")


def canonical(o):
    return json.dumps(o, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def hkdf64(key, info):
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF
    from cryptography.hazmat.primitives import hashes
    return HKDF(hashes.SHA256(), 64, b"\0" * 32, info.encode()).derive(key)


def aes_ctr(key, iv, data):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    e = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
    return e.update(data) + e.finalize()


def ssss_encrypt(key, name, secret):
    k = hkdf64(key, name)
    iv = bytearray(os.urandom(16))
    iv[8] &= 0x7F
    ct = aes_ctr(k[:32], bytes(iv), secret.encode())
    return {"iv": b64(bytes(iv)), "ciphertext": b64(ct), "mac": b64(hmac.new(k[32:], ct, hashlib.sha256).digest())}


class Api:
    def __init__(self, ctx, token, user):
        self.base, self.token, self.user = ctx.server_url, token, user

    def call(self, method, path, body=None):
        req = urllib.request.Request(self.base + "/_matrix/client/v3" + path, method=method,
                                     data=json.dumps(body).encode() if body is not None else None,
                                     headers={"Authorization": "Bearer " + self.token,
                                              "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req) as r:
                return r.status, json.load(r)
        except urllib.error.HTTPError as e:
            return e.code, json.load(e)

    def account_data(self, typ, content):
        u = urllib.parse.quote(self.user)
        return self.call("PUT", f"/user/{u}/account_data/{typ}", content)


async def peer(ctx):
    import olm
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
    from cryptography.hazmat.primitives import serialization

    me = ctx.target_user
    p = await ctx.peer(me)                       # a second device of the client's user
    api = Api(ctx, p.c.access_token, me.user_id)
    loop = asyncio.get_running_loop()
    run = lambda f, *a: loop.run_in_executor(None, f, *a)

    # 1. wait for the client's device, and make sure it never gets a room key from us
    client_dev = None
    end = time.time() + 180 * ctx.scale
    while time.time() < end and not client_dev:
        await p.refresh_devices(me.user_id)
        devs = p.c.device_store[me.user_id] if me.user_id in p.c.device_store.users else {}
        for d in devs.values():
            if d.id != p.c.device_id:
                client_dev = d
        if not client_dev:
            await asyncio.sleep(2)
    ctx.check(client_dev is not None, "client device seen")
    if not client_dev:
        return
    p.c.blacklist_device(client_dev)

    # 2. a room with messages the client cannot decrypt (no key shared with it)
    room = await p.create_room(encrypted=True, name="Vor meiner Zeit")
    for m in MESSAGES:
        await p.send(room, m)
    await p.sync(500)

    # 3. key backup (curve25519-aes-sha2): key pair, version, the room's sessions
    priv = X25519PrivateKey.generate()
    priv_raw = priv.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                  serialization.NoEncryption())
    pub = b64u(priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw))
    st, r = await run(api.call, "POST", "/room_keys/version",
                      {"algorithm": "m.megolm_backup.v1.curve25519-aes-sha2", "auth_data": {"public_key": pub}})
    ctx.check(st == 200, f"backup version created ({st})")
    version = r.get("version")
    enc = olm.PkEncryption(pub)
    ident = p.c.olm.account.identity_keys
    sessions = [s for s in p.c.olm.inbound_group_store if s.room_id == room]
    uploaded = 0
    for s in sessions:
        data = {"algorithm": "m.megolm.v1.aes-sha2", "sender_key": ident["curve25519"],
                "session_key": s.export_session(s.first_known_index),
                "sender_claimed_keys": {"ed25519": ident["ed25519"]}, "forwarding_curve25519_key_chain": []}
        m = enc.encrypt(json.dumps(data))
        body = {"first_message_index": s.first_known_index, "forwarded_count": 0, "is_verified": True,
                "session_data": {"ephemeral": m.ephemeral_key, "ciphertext": m.ciphertext, "mac": m.mac}}
        st, _ = await run(api.call, "PUT", f"/room_keys/keys/{urllib.parse.quote(room)}/"
                          f"{urllib.parse.quote(s.id, safe='')}?version={version}", body)
        uploaded += st == 200
    ctx.check(sessions and uploaded == len(sessions), f"{uploaded} room key(s) in the backup")

    # 4. cross-signing keys, uploaded with user-interactive auth
    seeds = {k: olm.PkSigning.generate_seed() for k in ("master", "self_signing", "user_signing")}
    signers = {k: olm.PkSigning(v) for k, v in seeds.items()}
    keyobj = lambda usage, s: {"user_id": me.user_id, "usage": [usage],
                               "keys": {"ed25519:" + s.public_key: s.public_key}}
    master = keyobj("master", signers["master"])
    ssk = keyobj("self_signing", signers["self_signing"])
    usk = keyobj("user_signing", signers["user_signing"])
    for o in (ssk, usk):
        o["signatures"] = {me.user_id: {"ed25519:" + signers["master"].public_key:
                                        signers["master"].sign(canonical(o))}}
    body = {"master_key": master, "self_signing_key": ssk, "user_signing_key": usk}
    st, r = await run(api.call, "POST", "/keys/device_signing/upload", body)
    if st == 401:
        body["auth"] = {"type": "m.login.password", "session": r.get("session"),
                        "identifier": {"type": "m.id.user", "user": me.localpart}, "password": me.password}
        st, r = await run(api.call, "POST", "/keys/device_signing/upload", body)
    ctx.check(st == 200, f"cross-signing keys uploaded ({st} {r if st != 200 else ''})")

    # 5. secret storage, the default key last (the client waits for it)
    k = hkdf64(SSSS_KEY, "")
    iv = bytearray(os.urandom(16))
    iv[8] &= 0x7F
    ct = aes_ctr(k[:32], bytes(iv), b"\0" * 32)
    key_info = {"algorithm": "m.secret_storage.v1.aes-hmac-sha2", "iv": b64(bytes(iv)),
                "mac": b64(hmac.new(k[32:], ct, hashlib.sha256).digest()),
                "passphrase": {"algorithm": "m.pbkdf2", "salt": SALT, "iterations": ITERATIONS, "bits": 256}}
    kid = "amxtestkey"
    secrets = {"m.cross_signing.master": b64u(seeds["master"]),
               "m.cross_signing.self_signing": b64u(seeds["self_signing"]),
               "m.cross_signing.user_signing": b64u(seeds["user_signing"]),
               "m.megolm_backup.v1": b64u(priv_raw)}
    ok = (await run(api.account_data, f"m.secret_storage.key.{kid}", key_info))[0] == 200
    for name, val in secrets.items():
        ok &= (await run(api.account_data, name, {"encrypted": {kid: ssss_encrypt(SSSS_KEY, name, val)}}))[0] == 200
    ok &= (await run(api.account_data, "m.secret_storage.default_key", {"key": kid}))[0] == 200
    ctx.check(ok, "secret storage written (key, 4 secrets, default key)")
    ctx.diag(f"recovery key {RECOVERY_KEY}")

    # 6. the client signs its device: wait for the signature by our self-signing key
    want = "ed25519:" + signers["self_signing"].public_key
    signed = False
    end = time.time() + 240 * ctx.scale
    while time.time() < end and not signed:
        st, r = await run(api.call, "POST", "/keys/query", {"device_keys": {me.user_id: []}})
        dk = r.get("device_keys", {}).get(me.user_id, {}).get(client_dev.id, {})
        sig = dk.get("signatures", {}).get(me.user_id, {}).get(want)
        if sig:
            o = {k2: v for k2, v in dk.items() if k2 not in ("signatures", "unsigned")}
            try:
                olm.ed25519_verify(signers["self_signing"].public_key, canonical(o), sig)
                signed = True
            except olm.OlmVerifyError:
                ctx.diag("signature present but invalid")
                break
        else:
            await asyncio.sleep(3)
    ctx.check(signed, "client device signed by the self-signing key (verified, as Element sees it)")
