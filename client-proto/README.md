# Step 2: receive path in C (`client-proto/`)

`amatrix-recv` is the receive side of a Matrix E2EE client in plain C, built
on the same libolm 3.2.16 that runs on the Amiga (step 1) plus cJSON. It runs
on Linux/macOS for now. The OS-specific parts sit behind two small interfaces
so the code can move to the Amiga later:

- `src/http.h`: `http_request()`. Host: libcurl (`http_curl.c`). Amiga:
  bsdsocket.library + AmiSSL (step 4).
- `src/platform.h`: random bytes, files, sleep. Host: POSIX
  (`platform_posix.c`).

What it does:

1. Logs in with `m.login.password` (or reuses the saved login).
2. Creates an Olm account and uploads signed device keys, 50 signed one-time
   keys and a fallback key. Signing uses Matrix canonical JSON
   (`canonjson.c`). It tops up one-time keys as the server's count drops.
3. Runs a `/sync` loop that:
   - joins rooms it is invited to,
   - decrypts Olm to-device messages (pre-key messages create a new inbound
     session, normal ones use the stored session), checks
     sender/recipient/recipient_keys and stores `m.room_key` as a Megolm
     inbound session,
   - decrypts Megolm room messages and prints them. Messages whose key has
     not arrived yet are queued and retried after the next to-device batch.
4. Keeps login, account, Olm/Megolm sessions and the sync token as small text
   files in a store directory, so the device stays the same across restarts.

## Status

Tested against a local Synapse 1.164.1 (the same version as
matrix.org), with matrix-nio 0.26 + vodozemac as the sender. vodozemac
is the Rust crypto that current Element versions also use.

| What | Result |
|---|---|
| login, device keys + OTK + fallback key upload | works; the peer accepts our signatures |
| invite → join | works |
| Olm pre-key message → new session → `m.room_key` | works |
| Olm normal message with session restored from disk | works |
| Megolm decrypt, UTF-8 (umlauts, emoji) | works |
| restart with persisted store | works, same device |

### On AmigaOS (FS-UAE, Kickstart 3.1, 68030/68882)

`./build-amiga.sh` builds `build-amiga/amatrix-recv` (220 KB): the same
`recv.c`, with `http_sock.c` + `net.c` on bsdsocket.library and AmiSSL 5.27
for https and for the random source (`RAND_bytes`).

| What | Result |
|---|---|
| plain HTTP against local Synapse (bsdsocket from UAE) | 2/2 messages decrypted |
| https://localhost:8448 (TLS proxy, private test CA in AmiSSL:Certs) | 2/2 decrypted, umlauts converted to Latin-1 for the console |
| same without the test CA (negative test) | refused: "certificate of localhost not accepted: unable to get local issuer certificate" |
| packaged start script `amatrix-test` | 3/3 decrypted |

Logs in `results/`. On the Amiga, errors go to stdout because the
AmigaDOS 3.x shell cannot redirect stderr.

`amiga-package/make-package.sh` builds `amatrix-step2.lha` for a real Amiga
(binary, `amatrix-test` script, `LiesMich.txt`).

### Sending (echo bot) - 2026-11-02

`-e` answers every message from someone else with "Amiga hat empfangen: ...",
`-g TEXT` greets each room once after joining.  Two-way test
`tools/peer-chat.py` (greeting + 3 echoes must decrypt on the peer):

| Where | Result |
|---|---|
| Linux, socket HTTP | PASS, also under AddressSanitizer |
| FS-UAE, AmiSSL, https | PASS |
| FS-UAE, the package script `amatrix-test` | PASS |

Real test on the V4SA against matrix.org with Element, two-way
(2026-11-02, `results/v4sa-dfence-element-chat-2026-11-02.*`): greeting and
3 echoes readable in Element (marked as unverified device), 3 messages from
Element decrypted on the Amiga, amxdiag clean, no crash on exit.

First real test on the V4SA against matrix.org with Element
(receive only): Element sent its room key to the unverified Amiga device,
all messages after the join decrypted (`results/v4sa-dfence-element-2026-11-02.txt`).
"Memory header not located" on program exit seen once on the V4SA
(ApolloOS) in the first test, not reproduced since (amxdiag and the chat
test exit cleanly).

Not done yet: replay protection (event ID per Megolm message index),
`m.forwarded_room_key`, key requests, device verification and cross-signing
(step 3), sending (step 4+). The pickle key is a fixed string for now.

## Build and test

```sh
cd client-proto
make                      # needs libcurl dev files and a C++ compiler
```

Local test server (once):

```sh
python3 -m venv venv && venv/bin/pip install matrix-synapse "matrix-nio[e2e]"
venv/bin/python -m synapse.app.homeserver --server-name localhost \
    --config-path hs/homeserver.yaml --generate-config --report-stats=no
# in hs/homeserver.yaml: bind the listener to 127.0.0.1 only if there is no IPv6
venv/bin/synctl start hs/homeserver.yaml
venv/bin/register_new_matrix_user -c hs/homeserver.yaml -u alice -p testalice --no-admin http://127.0.0.1:8008
venv/bin/register_new_matrix_user -c hs/homeserver.yaml -u amiga -p testamiga --no-admin http://127.0.0.1:8008
```

Then:

```sh
PY=venv/bin/python tools/local-test.sh     # prints PASS
```

A fresh store means a fresh device. That device cannot read older messages in
rooms the account had already joined, and reports them as "could not be
decrypted". This is correct behaviour.

## Against a real server with Element

```sh
build/amatrix-recv -s store-dfence -v https://matrix.org USER PASSWORD
```

Then invite the user from Element and write in the room. Element only sends
room keys to devices it is willing to encrypt for. If it refuses an unverified
device (Invisible Crypto, MSC4153), that is step 3 (cross-signing).
