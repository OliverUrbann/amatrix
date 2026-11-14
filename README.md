# Amatrix

A native [Matrix](https://matrix.org) client for classic AmigaOS (68020 or
better, AmigaOS 3.x), with end-to-end encryption and without a proxy or
bridge.  It consists of a client library, `libamatrix`, and **Amatrix**, a
MUI chat client built on it.

## Features

- End-to-end encryption (Olm/Megolm, via a ported libolm), key backup and
  security key, so that encrypted history is readable
- Direct chats, rooms, spaces, invitations, room settings, members with
  roles, knocking, room upgrades
- Replies, edits, deletions, threads, reactions, pinned messages, polls,
  formatted messages, Markdown while typing
- Pictures, files and voice messages, link previews, locations, emojis
- Typing notices and read receipts, search, export as text or HTML, drafts,
  history kept locally
- Notifications through [Herald](https://aminet.net/package/util/cdity/Herald),
  an AppIcon when iconified
- A password for the stored data (AES-256), several sessions, own profile
- An ARexx port (`AMATRIX.1`)
- English, Deutsch, Français, Español, Italiano, Nederlands and Svenska
  (locale.library catalogs)

## Requirements

- 68020 or better, an FPU is recommended
- MUI 5 with NList.mcc and BetterString.mcc
- AmiSSL 5 and a TCP/IP stack (bsdsocket.library)
- Optional: Herald, openurl.library, opusdec (voice messages)

## Installation

Download `Amatrix.lha` (with `Amatrix.readme`, the manual in English and
German), unpack it and double click `Amatrix`.  At the first start enter the
server (for example `https://matrix.org`), your user name and password.

## Building

The Amiga build needs the m68k-amigaos toolchain (gcc and libnix, from
<https://github.com/AmigaPorts/amiga-gcc>) in `/opt/amiga`, the Linux build
needs OpenSSL headers.  The MUI SDK is fetched with
`third_party/fetch-mui.sh`.

```sh
make                    # Linux: build/native/{libamatrix.a,tests/*,tools/*}
make TARGET=asan        # Linux with sanitizers
make TARGET=amiga       # Amiga: build/amiga/...
make TARGET=amiga gui   # the MUI client: build/amiga/gui/Amatrix
tools/amiga-package/make-gui-package.sh   # build/amiga/Amatrix.lha + Amatrix.readme
```

## Layout

| Path | What |
|---|---|
| `core/` | the client library, `libamatrix.a`; API in `core/include/amatrix.h`, for GUIs `amatrix_async.h` (network process) |
| `gui/` | **Amatrix**, the MUI client (MUI 5, NList, BetterString) |
| `examples/minimal.c` | the smallest client: room list and directory, or open a room (history, send, receive) - start here |
| `tools/` | `amx-cli` (command-line client), `amxdiag` (network/AmiSSL diagnosis), `catalog.py`, `amiga-package/` (packaging) |
| `tests/` | feature tests and their runner, see [tests/README.md](tests/README.md) |
| `catalogs/` | the languages of the client, see below |
| `libolm-amiga/` | libolm on AmigaOS: patches, unit tests, benchmark, see [its README](libolm-amiga/README.md) |
| `third_party/` | libolm 3.2.16 (patched), cJSON, AmiSSL SDK, stb_image; licences in [third_party/NOTICE.md](third_party/NOTICE.md) |
| `docs/` | [feature list](docs/matrix-library-features.md) |

## Languages

The texts of the client are English in the program; other languages come
from locale.library catalogs, `Catalogs/<language>/amatrix.catalog` next to
the program (or in `LOCALE:Catalogs/`).  Amatrix follows the languages in
the Locale preferences; `Amatrix LANGUAGE=english` (or the tool type
`LANGUAGE=deutsch`, ...) overrides them.

- `catalogs/amatrix.cd` - every text with its ID, in English
- `catalogs/<language>.ct` - the translations
- `tools/catalog.py` - makes `gui/amatrix_strings.h` and the `.catalog`
  files (FlexCat is not needed; the files use FlexCat's `.cd`/`.ct` layout)

To add a language, copy `deutsch.ct` to `<language>.ct` (the Amiga name,
e.g. `francais`), set `## language`, translate the line under each ID and
build; the package takes the catalog along.  `tools/catalog.py check
catalogs/amatrix.cd catalogs/<language>.ct` refuses a translation whose
`%s`/`%d` differ from the original.  New texts in the program are added at
the end of `amatrix.cd` (IDs never change) and used with `tr(MSG_...)`.
Corrections of the translations are welcome.

## Tests

```sh
tests/run.py [--target native|asan|amiga] [feature ...]
tests/gui/run_gui.py                     # the MUI client, in FS-UAE
```

The runner starts its own private Synapse, registers fresh users and runs
each feature's C test against an independent E2EE client.  Details in
[tests/README.md](tests/README.md).
