# Tests

Feature tests: each feature is exercised end to end against a real Matrix homeserver, from
our C core on one side and an independent client (matrix-nio, the same
vodozemac/Olm crypto as Element) on the other.

```sh
tests/run.py                         # all features, Linux build
tests/run.py --target asan           # Linux with AddressSanitizer/UBSan
tests/run.py --target amiga          # the Amiga build, inside FS-UAE
tests/run.py --target os4            # the AmigaOS 4 build, inside FS-UAE + QEMU-UAE
tests/run.py --target mos            # the MorphOS build, inside QEMU (pegasos2)
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
| `emu/` | setting up the FS-UAE environment (AmiSSL); `amxrun.c` + `remote.py`: the OS 4 runner |

A test program is called as `test SERVER USER PASSWORD PEER_USER STORE [EXTRA...]`
and talks to its peer only through Matrix (invites, rooms, messages).  Both
sides print TAP; the feature passes if both pass, the program exits 0 and no
sanitizer error shows up.  On the Amiga the store is `RAM:amx-store` and
timeouts are tripled.

## GUI test (MUI client in FS-UAE)

```sh
tests/emu/setup-emu-mui.sh           # once: MUI 5 + NList etc. into the emulator, 256-colour screen
tests/gui/run_gui.py [-v]            # builds Amatrix, runs the chat scenario, screenshots
tests/gui/run_gui.py --screenshot    # only the README screenshot: screenshots/amatrix.png (English)
```

The emulated Amiga runs Amatrix and a small AmigaDOS loop that executes
ARexx scripts (port `AMATRIX.1`) the host drops into `Extra:`; the peer chats
with it from the host; the host takes the screenshots (`xwd` on the Xvfb
display) into `build/amiga/gui-shots/`.  Checks are TAP lines in
`build/amiga/gui-test.log`.  Lessons: MUI names the port `<BASE>.1`;
ARexx host commands must be quoted strings; a nested `Execute` inside the
running autotest script ends it; console output opens the CLI window in
front of the GUI (redirect the program's output).

```sh
tests/gui/run_gui.py --target os4    # the same scenario in AmigaOS 4.1 FE (MUI 5 + NList installed)
tests/gui/run_gui.py --target mos    # ... in MorphOS 3.21 (QEMU pegasos2)
```

There `Extra:` is `RAM:Extra` in the guest, mirrored by `amxrun` (see
below): each ARexx script runs as one job together with a listing of
`Extra:`, and the files Amatrix writes are fetched back from that.  A
command that hangs twice in a row ends the scenario (a crashed Amatrix
keeps its ARexx port); the system's crash log goes to
`build/<target>/gui-crash.txt`.  If the emulator itself dies (QEMU-UAE
has been seen to segfault), the scenario ends at once and the whole test
runs once more.

## Adding a feature test

1. `tests/features/<name>/test.c`: `#include "mxtest.h"`, `mxt_args()`,
   `mxt_client()`, checks with `MXT_OK`, end with `return mxt_done();`.
2. `tests/features/<name>/peer.py`: `async def peer(ctx)`, `p = await ctx.peer()`,
   checks with `ctx.check(cond, "name")`.  Use `ctx.scale` for waits.

## Requirements

- Linux with a C compiler, `make`, `openssl`, Python 3.
- The Python environment (Synapse 1.164.1, matrix-nio[e2e]) is created on
  first use in `~/.cache/amatrix/venv` (`AMX_VENV` overrides).
- `--target amiga`: the m68k-amigaos toolchain in `/opt/amiga` and the
  FS-UAE test environment (`TESTENV`, default `~/amiga/testenv/amiga-testenv`) with AmiSSL installed by `emu/setup-emu-amissl.sh`.
  Kickstart/Workbench are not part of the repository.
- `--target os4`: the ppc-amigaos toolchain in `/opt/ppc-amigaos` and the
  AmigaOS 4.1 FE environment (`OS4ENV`, an `os41fe.hdf` with AmiSSL 5.27 installed
  by `emu/setup-os4-amissl.sh`).  The OS 4 system stays up for the whole run:
  `amxrun` in the emulator fetches the test programs and certificates from
  `remote.py` over the emulated network (the host is `10.0.2.2` there) and
  posts their output back (`--target mos`: MorphOS 3.21 in QEMU pegasos2,
  `MOSENV` = Emulation/MorphOS-macOS, amxrun comes on a CD, the CA via
  `SSL_CERT_FILE`).  Files carry a CRC32 (the emulated A2065/SLIRP
  network now and then corrupts data; a damaged file is sent again), and a
  test during which the emulated network dies completely is run once more
  on a freshly started system.  A hanging test leaves a screenshot and the
  kernel crash log (`DumpDebugBuffer`) in `build/os4/test-logs/`.

## Lessons learned (keep in mind when writing peers)

- Synapse >= 1.12x lets only admins publish rooms to the directory by
  default; the test server allows it (`room_list_publication_rules`).
- `amiga` is a predefined macro of m68k-amigaos-gcc: never use it as a name.
- nio's `room.users` includes invited users: wait with `Peer.wait_member`,
  which checks the real join, before sending; otherwise the room key is
  shared before the (slow, emulated) client is in the room.
- nio caches device lists: `Peer.refresh_devices(user)` before sending to a
  device that may have appeared late.
