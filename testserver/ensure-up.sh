#!/bin/bash
# Start the local Synapse and the TLS proxy if they are not running.
here=$(cd "$(dirname "$0")" && pwd)
curl -sf -m 3 http://127.0.0.1:8008/_matrix/client/versions >/dev/null || {
  rm -f "${AMX_DATA:-$HOME/amatrix-synapse}/homeserver.pid"; "$here/start.sh"; }
curl -sf -m 3 --cacert "$HOME/amatrix-tls/ca.pem" https://localhost:8448/_matrix/client/versions >/dev/null || {
  nohup python3 "$here/tlsproxy.py" "$HOME/amatrix-tls/srv.pem" "$HOME/amatrix-tls/srv.key" \
    > "$HOME/amatrix-tls/proxy.log" 2>&1 & sleep 1; echo "tls proxy started"; }
