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
#   ./scripts/backup-node.sh (--port /dev/cu.usbmodemXXXX | --host HOST[:PORT]) [--out DIR]

set -euo pipefail
umask 077 # exports contain the node's private identity key

PORT="${MESHTASTIC_PORT:-}"
HOST="${MESHTASTIC_HOST:-}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${MESHTASTIC_BACKUP_DIR:-$REPO_ROOT/hil-artifacts/backups/$(date +%F)}"
CLI="${MESHTASTIC_CLI:-}"
ATTEMPTS="${MESHTASTIC_BACKUP_ATTEMPTS:-4}"
TIMEOUT="${MESHTASTIC_BACKUP_TIMEOUT:-90}"

[[ "$ATTEMPTS" =~ ^[1-9][0-9]*$ ]] || { echo "MESHTASTIC_BACKUP_ATTEMPTS must be a positive integer" >&2; exit 2; }
[[ "$TIMEOUT" =~ ^[1-9][0-9]*$ ]] || { echo "MESHTASTIC_BACKUP_TIMEOUT must be a positive integer" >&2; exit 2; }

while [ $# -gt 0 ]; do
    case "$1" in
        --port) [ "$#" -ge 2 ] || { echo "--port needs a value" >&2; exit 2; }; PORT="$2"; shift 2 ;;
        --host) [ "$#" -ge 2 ] || { echo "--host needs a value" >&2; exit 2; }; HOST="$2"; shift 2 ;;
        --out)  [ "$#" -ge 2 ] || { echo "--out needs a value" >&2; exit 2; }; OUT="$2";  shift 2 ;;
        --cli)  [ "$#" -ge 2 ] || { echo "--cli needs a value" >&2; exit 2; }; CLI="$2";  shift 2 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -n "$PORT" ] && [ -n "$HOST" ]; then
    echo "choose exactly one transport: --port or --host" >&2
    exit 2
fi

if [ -z "$PORT" ] && [ -z "$HOST" ]; then
    shopt -s nullglob
    ports=(/dev/cu.usbmodem*)
    shopt -u nullglob
    if [ "${#ports[@]}" -ne 1 ]; then
        echo "refusing to guess the node; choose --port or MESHTASTIC_PORT" >&2
        printf '  %s\n' "${ports[@]}" >&2
        exit 2
    fi
    PORT="${ports[0]}"
fi

if [ -n "$PORT" ]; then
    [ -e "$PORT" ] || { echo "serial device does not exist: $PORT" >&2; exit 2; }
    CONN=(--port "$PORT")
else
    CONN=(--host "$HOST")
fi

if [ -n "$CLI" ]; then
    [ -x "$CLI" ] || { echo "MESHTASTIC_CLI is not executable" >&2; exit 2; }
else
    CLI="$(command -v meshtastic || true)"
    [ -n "$CLI" ] || { echo "meshtastic CLI not found (set MESHTASTIC_CLI)" >&2; exit 2; }
fi
command -v perl >/dev/null 2>&1 || { echo "perl is required to enforce backup timeouts" >&2; exit 2; }

mkdir -p "$OUT"
STAMP=$(date +%H%M%S)

# macOS has no coreutils timeout. An alarm survives exec(), so this keeps the
# deadline and the CLI in one process without polling PIDs or leaving watchdogs.
run_capped() {
    local secs="$1"; shift
    local dest="$1"; shift
    local status=0
    perl -e 'alarm shift; exec @ARGV' "$secs" "$@" > "$dest" 2>/dev/null || status=$?
    if [ "$status" -eq 142 ]; then
        return 124
    fi
    return "$status"
}

valid_full_export() {
    local path="$1"
    grep -Eq '^channel_url:[[:space:]]+[^[:space:]]' "$path" &&
        grep -Eq '^config:[[:space:]]*$' "$path" &&
        grep -Eq '^module_config:[[:space:]]*$' "$path" &&
        grep -Eq '^owner:[[:space:]]+[^[:space:]]' "$path" &&
        # Meshtastic 2.7 exports an opaque scalar that is not guaranteed to use
        # the standard Base64 alphabet (current keys can contain a separator).
        # Require a substantial, single-token value without trying to decode a
        # format owned by the upstream CLI.
        grep -Eq '^[[:space:]]+privateKey:[[:space:]]+[^[:space:]]{16,}[[:space:]]*$' "$path"
}

YAML="$OUT/config-$STAMP.yaml"
ok=

for attempt in $(seq 1 "$ATTEMPTS"); do
    echo "export-config, attempt $attempt/$ATTEMPTS ..."
    export_status=0
    run_capped "$TIMEOUT" "$YAML" "$CLI" "${CONN[@]}" --export-config || export_status=$?
    if [ "$export_status" -ne 0 ]; then
        if [ "$export_status" -eq 124 ]; then
            echo "  timed out after ${TIMEOUT}s (a lost admin response — retrying)"
        else
            echo "  meshtastic CLI exited with $export_status — retrying"
        fi
        continue
    fi
    # A zero exit and one key line are not enough: reject truncated exports that
    # cannot restore channels/config/modules even when their first section survived.
    if valid_full_export "$YAML" 2>/dev/null; then
        ok=1
        break
    fi
    echo "  returned but is incomplete — not a restorable backup, retrying"
done

if [ -n "$ok" ]; then
    echo
    echo "OK  $YAML  ($(wc -c < "$YAML" | tr -d ' ') bytes)"
    grep -E "^(owner|owner_short):" "$YAML" || true
    echo
    echo "Restore with:  $CLI ${CONN[*]} --configure $YAML"
    exit 0
fi

# Every export attempt failed. The private key alone is what cannot be reconstructed
# from notes, and asking for it directly avoids the calls that hang, so take that much
# rather than leaving with nothing.
echo
echo "export-config did not succeed in $ATTEMPTS attempts — falling back to the key alone"
SEC="$OUT/security-$STAMP.txt"
run_capped 60 "$SEC" "$CLI" "${CONN[@]}" --get security || true
if grep -q "private_key" "$SEC" 2>/dev/null; then
    echo "PARTIAL  $SEC  — holds the node identity, but not the rest of the settings"
    echo "Run this script again for a full config; do NOT erase flash until you have one."
    exit 1
fi

echo "FAILED — no backup was captured. Do not erase this node's flash." >&2
exit 1
