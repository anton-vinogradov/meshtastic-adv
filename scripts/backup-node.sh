#!/usr/bin/env bash
# Capture a restorable backup of a node's configuration.
#
# The reason this is not a one-liner around `meshtastic --export-config`: that
# command waits for admin responses in an unbounded loop, and this link loses one
# every so often (roughly one run in five here). When it does, the command never
# returns — and if you were redirecting stdout, you are left with a zero-byte file
# that looks exactly like a successful backup. That is how a node's identity was
# lost once already: the private key lives only in flash, and an erase without a
# good backup takes the node number with it.
#
# So: bounded attempts, and nothing is called a backup until it has been read back
# and found to contain a private key.
#
#   ./scripts/backup-node.sh [--port /dev/cu.usbmodemXXXX] [--out DIR]

set -euo pipefail

PORT="${MESHTASTIC_PORT:-/dev/cu.usbmodem2101}"
OUT="$HOME/claude/meshtastic-backups/$(date +%F)"
CLI="${MESHTASTIC_CLI:-$HOME/.pio-core-venv/bin/meshtastic}"
ATTEMPTS=4
TIMEOUT=90

while [ $# -gt 0 ]; do
    case "$1" in
        --port) PORT="$2"; shift 2 ;;
        --out)  OUT="$2";  shift 2 ;;
        --cli)  CLI="$2";  shift 2 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -x "$CLI" ] || CLI=meshtastic
command -v "$CLI" >/dev/null 2>&1 || { echo "meshtastic CLI not found (set MESHTASTIC_CLI)" >&2; exit 2; }

mkdir -p "$OUT"
STAMP=$(date +%H%M%S)

# macOS has no coreutils timeout; run detached and reap it ourselves.
run_capped() {
    local secs="$1"; shift
    local dest="$1"; shift
    "$@" > "$dest" 2>/dev/null &
    local pid=$!
    local waited=0
    while kill -0 "$pid" 2>/dev/null; do
        [ "$waited" -ge "$secs" ] && { kill -9 "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; return 1; }
        sleep 1
        waited=$((waited + 1))
    done
    wait "$pid" 2>/dev/null || true
    return 0
}

YAML="$OUT/config-$STAMP.yaml"
ok=

for attempt in $(seq 1 "$ATTEMPTS"); do
    echo "export-config, attempt $attempt/$ATTEMPTS ..."
    run_capped "$TIMEOUT" "$YAML" "$CLI" --port "$PORT" --export-config || {
        echo "  timed out after ${TIMEOUT}s (a lost admin response — retrying)"
        continue
    }
    # An export that returned is still not a backup until it proves it holds the key.
    if grep -q "privateKey:" "$YAML" 2>/dev/null; then
        ok=1
        break
    fi
    echo "  returned but has no privateKey — not a usable backup, retrying"
done

if [ -n "$ok" ]; then
    echo
    echo "OK  $YAML  ($(wc -c < "$YAML" | tr -d ' ') bytes)"
    grep -E "^(owner|owner_short):" "$YAML" || true
    echo
    echo "Restore with:  $CLI --port $PORT --configure $YAML"
    exit 0
fi

# Every export attempt failed. The private key alone is what cannot be reconstructed
# from notes, and asking for it directly avoids the calls that hang, so take that much
# rather than leaving with nothing.
echo
echo "export-config did not succeed in $ATTEMPTS attempts — falling back to the key alone"
SEC="$OUT/security-$STAMP.txt"
run_capped 60 "$SEC" "$CLI" --port "$PORT" --get security || true
if grep -q "private_key" "$SEC" 2>/dev/null; then
    echo "PARTIAL  $SEC  — holds the node identity, but not the rest of the settings"
    echo "Run this script again for a full config; do NOT erase flash until you have one."
    exit 1
fi

echo "FAILED — no backup was captured. Do not erase this node's flash." >&2
exit 1
