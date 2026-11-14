# libolm on AmigaOS (`libolm-amiga/`)

libolm 3.2.16 (Olm/Megolm, the crypto behind Matrix E2EE) built for
AmigaOS 3.x with libnix, 68020 + FPU. No ixemul, no AmiSSL: libolm brings its
own Curve25519, Ed25519, AES and SHA-256.

`libolm.a` is about 190 KB; the 15 libolm unit tests and the benchmark
program `olmbench` run on AmigaOS 3.x.

## Build

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

## Running on the Amiga

Copy `out/amiga/bin/` to the Amiga, then in a Shell in that drawer:

```
Execute run-tests
```

Results are written to `RAM:olm-results.txt`. `olmbench [scale]` alone times
the operations a client needs (key generation, Ed25519 sign/verify, Olm
sessions, Megolm encrypt/decrypt, SAS).

Requirements: 68020+ with FPU (the test framework and the benchmark use
`double`; libolm itself does not), about 256 KB stack (the script sets it).

## Porting notes

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
