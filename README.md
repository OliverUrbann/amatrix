# Amatrix

Feasibility work for a native Matrix client on classic AmigaOS (68k), with
end-to-end encryption and without a proxy.

## Step 1: libolm on AmigaOS (`libolm-amiga/`)

libolm 3.2.16 (Olm/Megolm, the crypto behind Matrix E2EE) built for
AmigaOS 3.x with libnix, 68020 + FPU. No ixemul, no AmiSSL: libolm brings its
own Curve25519, Ed25519, AES and SHA-256.

Status:

| What | Result |
|---|---|
| libolm builds for m68k-amigaos | yes, `libolm.a` about 190 KB |
| libolm unit tests (15 programs) in FS-UAE (A1200, 68030/68882, KS 3.1) | all pass |
| `olmbench` in FS-UAE | runs, results correct |
| unit tests and `olmbench` on a real Vampire V4SA | to do |

### Build

Needs the m68k-amigaos toolchain (gcc 16.2 + libnix, from
<https://github.com/AmigaPorts/amiga-gcc>) under `/opt/amiga`.

```sh
export PATH=/opt/amiga/bin:$PATH
libolm-amiga/build.sh amiga     # -> libolm-amiga/out/amiga/{libolm.a,bin/}
libolm-amiga/build.sh native    # same tests on the Linux host, for reference
```

`third_party/libolm` is libolm 3.2.16 with our patches already applied
(`libolm-amiga/patches/`). `libolm-amiga/fetch-libolm.sh` recreates it from
the `python-olm` sdist on PyPI, which carries the same source tree as the
upstream repository on gitlab.matrix.org.

### Running on the Amiga

Copy `out/amiga/bin/` to the Amiga, then in a Shell in that drawer:

```
Execute run-tests
```

Results are written to `RAM:olm-results.txt`. `olmbench [scale]` alone times
the operations a client needs (key generation, Ed25519 sign/verify, Olm
sessions, Megolm encrypt/decrypt, SAS).

Requirements: 68020+ with FPU (the test framework and the benchmark use
`double`; libolm itself does not), about 256 KB stack (the script sets it).

### Porting notes

- `uint32_t` is `unsigned long` on m68k-amigaos. The bundled
  crypto-algorithms define `WORD` as `unsigned int`, which C++ rejects when
  libolm passes `uint32_t*`. Patch 0001 makes `WORD` a `uint32_t`.
- The test framework (doctest 2.4.7) needs a no-op `std::mutex`
  (`compat/doctest_amiga.h`), `DOCTEST_CONFIG_NO_POSIX_SIGNALS` and a
  single-threaded `__atomic_fetch_add_4` (`compat/atomic.c`). libolm itself
  needs none of this.
- libstdc++ in the tests opens `mathieeedoubbas`/`mathieeedoubtrans.library`.
  The shareware HSMathLibs replacements pop up a "please register" requester
  that blocks the test run; the original Workbench 3.1 libraries work.
- Big-endian: no issues, all test vectors match.

## Next steps

1. Run the tests and `olmbench` on the Vampire V4SA.
2. Receive path on a Linux PC first (C + libolm + libcurl): upload device and
   one-time keys, receive an `m.room_key` via Olm, decrypt a Megolm room
   message from Element. Against our own homeserver.
3. Cross-signing if Element does not send keys to an unverified device.
4. Move that code to the Amiga (bsdsocket + AmiSSL).
