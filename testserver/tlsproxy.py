#!/usr/bin/env python3
"""TLS front for the local test Synapse: https://localhost:8448 -> http://127.0.0.1:8008.
Certificate from a private test CA (see tls-setup in README), so the Amiga's
AmiSSL can verify the chain and the host name exactly as against a real server.
  tlsproxy.py <cert.pem> <key.pem> [listen_port] [backend_port]"""
import asyncio, ssl, sys

cert, key = sys.argv[1], sys.argv[2]
lport = int(sys.argv[3]) if len(sys.argv) > 3 else 8448
bport = int(sys.argv[4]) if len(sys.argv) > 4 else 8008

async def pipe(r, w):
    try:
        while data := await r.read(65536):
            w.write(data)
            await w.drain()
    except Exception:
        pass
    finally:
        try: w.close()
        except Exception: pass

async def handle(cr, cw):
    try:
        br, bw = await asyncio.open_connection("127.0.0.1", bport)
    except Exception:
        cw.close(); return
    await asyncio.gather(pipe(cr, bw), pipe(br, cw))

async def main():
    ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    ctx.load_cert_chain(cert, key)
    srv = await asyncio.start_server(handle, "127.0.0.1", lport, ssl=ctx)
    print(f"tls proxy on 127.0.0.1:{lport} -> {bport}", flush=True)
    async with srv:
        await srv.serve_forever()

asyncio.run(main())
