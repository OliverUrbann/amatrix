#!/bin/bash
data=${AMX_DATA:-$HOME/amatrix-synapse}; venv=${AMX_VENV:-$HOME/amiga/venv}
cd "$data" && "$venv/bin/python" -m synapse.app.homeserver -c homeserver.yaml --daemonize
for i in $(seq 30); do curl -sf http://127.0.0.1:8008/_matrix/client/versions >/dev/null && { echo "synapse up"; exit 0; }; sleep 1; done
echo "synapse did not start, see $data/homeserver.log"; exit 1
