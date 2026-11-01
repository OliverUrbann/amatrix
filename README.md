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
| unit tests on a real Vampire V4SA (Kickstart 45.64) | all pass |
| `olmbench` on the V4SA | runs, results correct (see below) |

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

### olmbench on the Vampire V4SA

Full output: `libolm-amiga/results/v4sa-2026-11-01.txt`. Times per operation,
plain C build (`-m68020 -m68881 -O2`), no assembler, no tuning:

| Operation | V4SA | FS-UAE (68030, max speed) |
|---|---:|---:|
| create account (2 keypairs) | 50 ms | 90 ms |
| one-time key (Curve25519) | 36 ms | 65 ms |
| Ed25519 sign | 14 ms | 20 ms |
| Ed25519 verify (device keys) | 39 ms | 62 ms |
| create outbound Olm session | 181 ms | 340 ms |
| create inbound Olm session | 110 ms | 190 ms |
| Olm encrypt+decrypt | 156 ms | 263 ms |
| Megolm encrypt (send a message) | 16 ms | 28 ms |
| Megolm decrypt (receive a message) | 50 ms | 71 ms |
| Megolm ratchet jump (old history) | 109 ms | 161 ms |
| SAS key agreement (x2) | 71 ms | 122 ms |

What it means for a client:

- Live chat is no problem: receiving a message costs about 50 ms, sending 16 ms.
- Room keys arrive once per sender session (Olm, 100-200 ms), not per message.
- Costs add up in bulk: decrypting 1000 messages of history is about 50 s,
  checking 500 device signatures in a big room about 20 s. A client should
  decrypt only what is on screen and cache verified device keys.
- Room for speed-up: most of the time is Curve25519/Ed25519 field arithmetic
  in portable C with 64-bit products. 68k assembler for that inner loop (the
  68080 has a fast 32x32->64 multiply) is the obvious next optimisation.

## Next steps

1. Run the tests and `olmbench` on the Vampire V4SA.
2. Receive path on a Linux PC first (C + libolm + libcurl): upload device and
   one-time keys, receive an `m.room_key` via Olm, decrypt a Megolm room
   message from Element. Against our own homeserver.
3. Cross-signing if Element does not send keys to an unverified device.
4. Move that code to the Amiga (bsdsocket + AmiSSL).
