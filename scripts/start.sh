#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
RESOLVE_BIN="/opt/resolve/bin/resolve"
SESSION_RUNNER="$PROJECT_ROOT/installer/run_resolve_session.sh"

usage() {
    cat <<'USAGE'
Usage:
  start.sh [Resolve arguments...]
  start.sh --help

Starts Resolve with the codec runtime installed in that
same process. There is no stock/unpatched fallback path. The patch is applied
automatically on every normal start; diagnostic failures stop startup.

USAGE
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi

[[ -x "$RESOLVE_BIN" ]] || {
    printf 'Error: Resolve executable not found or not executable: %s\n' "$RESOLVE_BIN" >&2
    exit 1
}
[[ -x "$SESSION_RUNNER" ]] || {
    printf 'Error: Resolve session launcher is missing or not executable: %s\n' "$SESSION_RUNNER" >&2
    exit 78
}

exec "$SESSION_RUNNER" "$@"
