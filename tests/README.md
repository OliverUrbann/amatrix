# Tests

Feature tests on the level of [docs/matrix-library-features.md](../docs/matrix-library-features.md):
each feature is exercised end to end against a real Matrix homeserver, from
our C core on one side and an independent client (matrix-nio, the same
vodozemac/Olm crypto as Element) on the other.

```sh
tests/run.py                         # all features, Linux build
tests/run.py --target asan           # Linux with AddressSanitizer/UBSan
tests/run.py --target amiga          # the Amiga build, inside FS-UAE
tests/run.py e2ee_send rooms -v      # selected features, live output
AMX_VERBOSE=1 tests/run.py ...       # core logging in the test programs (2 = HTTP too)
```

`run.py` builds what it needs (`make TARGET=...`), starts **its own private
Synapse** (`server/server.py`, same version as matrix.org) in a
temporary directory, with an HTTPS front (private test CA) and a second HTTPS
front with an untrusted certificate, and stops and deletes it at the end
(`--keep` keeps it).  Nothing depends on any existing server.

Per feature it registers two fresh users, so features never share devices
or rooms.  Logs: `build/<target>/test-logs/<feature>.log`.

## Layout

| Path | What |
|---|---|
| `run.py` | the runner |
| `mxtest.h` | C side: TAP output (`MXT_OK`), recorded events, `mxt_sync_until` |
| `peerlib.py` | peer side: `Peer` (login, rooms, send, wait), `Context` (`ctx.check`) |
| `features/<name>/test.c` | our client under test, via the public API `amatrix.h` |
| `features/<name>/peer.py` | `async def peer(ctx)`: the other side; `TIMEOUT`, `EXTRA_ARGS` |
| `server/` | private Synapse + TLS proxy |
| `emu/` | setting up the FS-UAE environment (AmiSSL) |

A test program is called as `test SERVER USER PASSWORD PEER_USER STORE [EXTRA...]`
and talks to its peer only through Matrix (invites, rooms, messages).  Both
sides print TAP; the feature passes if both pass, the program exits 0 and no
sanitizer error shows up.  On the Amiga the store is `RAM:amx-store` and
timeouts are tripled.

## Adding a feature test

1. `tests/features/<name>/test.c`: `#include "mxtest.h"`, `mxt_args()`,
   `mxt_client()`, checks with `MXT_OK`, end with `return mxt_done();`.
2. `tests/features/<name>/peer.py`: `async def peer(ctx)`, `p = await ctx.peer()`,
   checks with `ctx.check(cond, "name")`.  Use `ctx.scale` for waits.
3. Add the test name to the "Test" column in the feature list.

## Requirements

- Linux with a C compiler, `make`, `openssl`, Python 3.
- The Python environment (Synapse 1.164.1, matrix-nio[e2e]) is created on
  first use in `~/.cache/amatrix/venv` (`AMX_VENV` overrides).
- `--target amiga`: the m68k-amigaos toolchain in `/opt/amiga` and the
  FS-UAE test environment (`TESTENV`, default `~/amiga/testenv/amiga-testenv`,
  from the amiga_rsync port) with AmiSSL installed by `emu/setup-emu-amissl.sh`.
  Kickstart/Workbench are not part of the repository.

## Lessons learned (keep in mind when writing peers)

- nio's `room.users` includes invited users: wait with `Peer.wait_member`,
  which checks the real join, before sending; otherwise the room key is
  shared before the (slow, emulated) client is in the room.
- nio caches device lists: `Peer.refresh_devices(user)` before sending to a
  device that may have appeared late.
