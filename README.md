# Amatrix

A native Matrix client core for classic AmigaOS (68020+, AmigaOS 3.x /
ApolloOS), with end-to-end encryption and without a proxy.  Goal: a GUI
client on top of it; the core may later also become a `matrix.library`.

**Using Amatrix:** the MUI client is packaged for Aminet as `Amatrix.lha`
with `Amatrix.readme` (English and German, the only documentation file):
`tools/amiga-package/make-gui-package.sh` builds both from
`tools/amiga-package/Amatrix.readme.txt` (fill in the uploader address
first).  Third-party licences: [third_party/NOTICE.md](third_party/NOTICE.md).

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
| reactions | ✔ | ✔ | ✔ |
| media | ✔ | ✔ | ✔ |
| replies | ✔ | ✔ | ✔ |
| send_media | ✔ | ✔ | ✔ |
| typing_receipts | ✔ | ✔ | ✔ |
| room_admin | ✔ | ✔ | ✔ |
| profile | ✔ | ✔ | ✔ |
| link_preview | ✔ | ✔ | ✔ |
| sessions | ✔ | ✔ | ✔ |
| history_cache | ✔ | ✔ | ✔ |
| organize | ✔ | ✔ | ✔ |
| polls | ✔ | ✔ | ✔ |
| message_tools | ✔ | ✔ | ✔ |
| notify_rules | ✔ | ✔ | ✔ |
| spaces_report | ✔ | ✔ | ✔ |
| tls_verify | ✔ | ✔ | ✔ |
| room_list | ✔ | ✔ | ✔ |
| directory | ✔ | ✔ | ✔ |
| history | ✔ | ✔ | ✔ |
| async | ✔ | ✔ | ✔ |
| avatars | ✔ | ✔ | ✔ |
| recovery | ✔ | ✔ | ✔ |
| store_crypt | ✔ | ✔ | ✔ |

## Layout

| Path | What |
|---|---|
| `core/` | the client core, `libamatrix.a`; API in `core/include/amatrix.h`, for GUIs `amatrix_async.h` (network process) |
| `gui/` | **Amatrix**, the MUI client (MUI 5, NList, BetterString): `make TARGET=amiga gui`, test `tests/gui/run_gui.py`, package `tools/amiga-package/make-gui-package.sh`; the SDK comes from `third_party/fetch-mui.sh` |
| `examples/minimal.c` | the smallest client: room list and directory, or open a room (history, send, receive) - start here |
| `tools/` | `amx-cli` (command-line test client), `amxdiag` (network/AmiSSL diagnosis), `amiga-package/` (test package for a real Amiga) |
| `tests/` | feature tests and their runner, see [tests/README.md](tests/README.md) |
| `libolm-amiga/` | libolm on AmigaOS: patches, unit tests, `olmbench`, see [its README](libolm-amiga/README.md) |
| `catalogs/` | languages of the GUI: `amatrix.cd` (English, built in), `<language>.ct` (translations), see below |
| `third_party/` | libolm 3.2.16 (patched), cJSON, AmiSSL 5.27 SDK, stb_image |
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

## Languages

The GUI's texts are English in the program and come from a locale.library
catalog for other languages: `Catalogs/<language>/amatrix.catalog` next to
the program (or in `LOCALE:Catalogs/`).  Amatrix follows the languages in
the Locale prefs; `Amatrix LANGUAGE=english` (or `deutsch`, ...) overrides
them.

- `catalogs/amatrix.cd` - every text with its ID, in English
- `catalogs/<language>.ct` - translations: `deutsch`, `francais`, `espanol`, `italiano`, `nederlands`, `svenska` (all but German are machine-assisted and not reviewed by native speakers)
- `tools/catalog.py` - makes `gui/amatrix_strings.h` and the `.catalog`
  files (no FlexCat needed; the files use FlexCat's `.cd`/`.ct` layout)

A new language: copy `deutsch.ct` to `<language>.ct` (the Amiga name, e.g.
`francais`), set `## language`, translate the line under each ID, then
`make TARGET=amiga gui` builds `build/amiga/catalogs/<language>/amatrix.catalog`
and the package takes it along.  `tools/catalog.py check catalogs/amatrix.cd
catalogs/<language>.ct` refuses a translation whose `%s`/`%d` differ from
the original.  New texts in the program: add them at the end of
`amatrix.cd` (IDs never change) and use `tr(MSG_...)`.

## Tests

```sh
tests/run.py [--target native|asan|amiga] [feature ...]
```

Starts its own Synapse, registers fresh users, runs each feature's C test
against an independent E2EE peer.  Details in [tests/README.md](tests/README.md).

## Known bugs

- **Picture previews have thin lines now and then.**  NList gives every
  entry the same height, so a preview is cut into strips of line height,
  one list entry each.  Sometimes NList leaves out the bottom row of a strip
  (mostly while scrolling; a full redraw shows the picture right), and a
  1-pixel line of background shows.  This is NList's drawing, not the
  picture: the GUI test checks that all strips are there in order and
  allows up to 3 missing rows.

## Next steps

See the [feature list](docs/matrix-library-features.md) (priorities P1-P3,
"Test" column).  Next: the remaining P1 features (message formats, media,
cross-signing and verification, sync details) with a feature test each, and
the asynchronous layer (sync in its own process) for the GUI.
