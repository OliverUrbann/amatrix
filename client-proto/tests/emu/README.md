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

`run-emu-test.sh <script> [peer...]` copies the binaries to an Extra: drawer,
starts the emulator, runs the peer and prints the Amiga's output.
Scripts are UTF-8 here; run-emu-test.sh converts them to ISO-8859-1.
