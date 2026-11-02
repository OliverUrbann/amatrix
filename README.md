# Amatrix

A native Matrix client core for classic AmigaOS (68020+, AmigaOS 3.x /
ApolloOS), with end-to-end encryption and without a proxy.  Goal: a GUI
client on top of it; the core may later also become a `matrix.library`.

**Status 2026-11-02:** a Vampire V4SA chats end-to-end encrypted with Element
on matrix.org (receive and send).  The core has been split out into a
library with an API and is covered by feature tests that run on Linux,
under AddressSanitizer and on the Amiga build inside FS-UAE, each against a
private Synapse started by the tests.

| Feature test | Linux | ASan | Amiga (FS-UAE) |
|---|---|---|---|
| login | ✔ | ✔ | ✔ |
| rooms | ✔ | ✔ | ✔ |
| e2ee_receive | ✔ | ✔ | ✔ |
| e2ee_send | ✔ | ✔ | ✔ |
| tls_verify | ✔ | ✔ | ✔ |

## Layout

| Path | What |
|---|---|
| `core/` | the client core, `libamatrix.a`; API in `core/include/amatrix.h` |
| `tools/` | `amx-cli` (command-line test client), `amxdiag` (network/AmiSSL diagnosis), `amiga-package/` (test package for a real Amiga) |
| `tests/` | feature tests and their runner, see [tests/README.md](tests/README.md) |
| `libolm-amiga/` | libolm on AmigaOS: patches, unit tests, `olmbench`, see [its README](libolm-amiga/README.md) |
| `third_party/` | libolm 3.2.16 (patched), cJSON, AmiSSL 5.27 SDK |
| `docs/` | [feature list](docs/matrix-library-features.md), [proof of concept](docs/proof-of-concept.md) |
| `results/` | logs of the tests on the real Vampire and earlier emulator runs |

## Build

Needs the m68k-amigaos toolchain (gcc 16.2 + libnix, from
<https://github.com/AmigaPorts/amiga-gcc>) in `/opt/amiga` for the Amiga
build, and OpenSSL headers for the Linux build.

```sh
make                    # Linux: build/native/{libamatrix.a,tests/*,tools/*}
make TARGET=asan        # Linux with sanitizers
make TARGET=amiga       # Amiga: build/amiga/...
tools/amiga-package/make-package.sh    # build/amiga/amatrix-test.lha
```

On the Amiga the programs need a TCP/IP stack (bsdsocket.library), AmiSSL 5.x
and a 68020 or better with FPU.

## Tests

```sh
tests/run.py [--target native|asan|amiga] [feature ...]
```

Starts its own Synapse, registers fresh users, runs each feature's C test
against an independent E2EE peer.  Details in [tests/README.md](tests/README.md).

## Next steps

See the [feature list](docs/matrix-library-features.md) (priorities P1-P3,
"Test" column).  Next: the remaining P1 features (message formats, media,
cross-signing and verification, sync details) with a feature test each, and
the asynchronous layer (sync in its own process) for the GUI.
