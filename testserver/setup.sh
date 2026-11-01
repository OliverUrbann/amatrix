#!/bin/bash
# Local Synapse for Amatrix tests (same version as matrix.org).
#   setup.sh          create venv + config (once)
#   start.sh / stop.sh
# Server name "amatrix.test", client API http://127.0.0.1:8008 (the emulated
# Amiga reaches it under the same address).  Users: alice, bob, amiga
# (password = username + "-pw"); alice is admin.
# Test with:  cd ../client-proto && DOMAIN=amatrix.test ALICE_PW=alice-pw \
#             AMIGA_PW=amiga-pw PY=$HOME/amiga/venv/bin/python sh tools/local-test.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
data=${AMX_DATA:-$HOME/amatrix-synapse}
venv=${AMX_VENV:-$HOME/amiga/venv}
[ -x "$venv/bin/python" ] || { python3 -m venv "$venv"; "$venv/bin/pip" install -q matrix-synapse==1.164.1; }
mkdir -p "$data"
if [ ! -f "$data/homeserver.yaml" ]; then
  "$venv/bin/python" -m synapse.app.homeserver --server-name amatrix.test \
     --config-path "$data/homeserver.yaml" --generate-config --report-stats=no \
     --data-directory "$data"
  cat >> "$data/homeserver.yaml" <<YAML

# --- Amatrix test settings ---
enable_registration: false
registration_shared_secret: "amatrix-test-secret"
rc_message: {per_second: 1000, burst_count: 1000}
rc_login:
  address: {per_second: 1000, burst_count: 1000}
  account: {per_second: 1000, burst_count: 1000}
  failed_attempts: {per_second: 1000, burst_count: 1000}
rc_registration: {per_second: 1000, burst_count: 1000}
rc_joins:
  local: {per_second: 1000, burst_count: 1000}
rc_key_requests: {per_second: 1000, burst_count: 1000}
rc_invites:
  per_room: {per_second: 1000, burst_count: 1000}
  per_user: {per_second: 1000, burst_count: 1000}
YAML
  # no IPv6 in the sandbox: listen on 127.0.0.1 only
  sed -i '/^    - ::1$/d' "$data/homeserver.yaml"
  echo 'suppress_key_server_warning: true' >> "$data/homeserver.yaml"
fi
"$here/start.sh"
for u in alice bob amiga; do
  admin=""; [ $u = alice ] && admin="-a" || admin="--no-admin"
  "$venv/bin/register_new_matrix_user" -u $u -p $u-pw $admin \
     -k amatrix-test-secret http://127.0.0.1:8008 2>&1 | grep -v "already taken" || true
done
