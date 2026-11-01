#!/bin/bash
data=${AMX_DATA:-$HOME/amatrix-synapse}
[ -f "$data/homeserver.pid" ] && kill "$(cat "$data/homeserver.pid")" && echo stopped
