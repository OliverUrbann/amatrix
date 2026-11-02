# Emulator tests (FS-UAE)

AmigaDOS scripts for `run-amiga.sh` from the amiga-testenv archive (FS-UAE,
A1200, KS 3.1, UAE bsdsocket).  The emulated Amiga reaches the host as
127.0.0.1; the local Synapse (`../../testserver`) is on http://127.0.0.1:8008,
the TLS proxy with the private test CA on https://localhost:8448.  The test
CA (`~/amatrix-tls/ca.pem`, hash name `1d4bd0f8.0`) and AmiSSL 5.27 must be in
the emulated system (`SYS:AmiSSL`, see `setup-emu-amissl.sh`).

| Script | What | Peer on the host |
|---|---|---|
| `recv-http.test` | receive over plain HTTP | `tools/peer.py` |
| `recv-tls.test` | receive over https/AmiSSL | `tools/peer.py` |
| `chat.test` | echo bot: greeting + echoes, two-way | `tools/peer-chat.py` |
| `package.test` | the `amatrix-test` script from the package | `tools/peer.py` |
| `diag.test` | `amxdiag-run`, no peer needed | - |
| `package-chat.test` | the package's `amatrix-test` (diag + echo bot) | `tools/peer-chat.py`, started ~45 s later |

Example (two-way test as in the package):

    MAX=420 tests/emu/run-emu-test.sh tests/emu/package-chat.test bash -c \
      'sleep 45; tools/peer-chat.py http://127.0.0.1:8008 @alice:amatrix.test alice-pw \
       @amiga:amatrix.test "Hallo vom Amiga! (Amatrix-Test, verschlüsselt gesendet)"'

Note: nio caches device lists.  The emulated 68030 needs several seconds to
create its keys, so peer-chat.py re-queries the target's devices before it
sends; otherwise nio does not share its room key with the new device.

`run-emu-test.sh <script> [peer...]` copies the binaries to an Extra: drawer,
starts the emulator, runs the peer and prints the Amiga's output.
Scripts are UTF-8 here; run-emu-test.sh converts them to ISO-8859-1.
