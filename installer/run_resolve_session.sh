#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
RESOLVE_BIN="/opt/resolve/bin/resolve"
GDB_BIN="${GDB:-gdb}"
CONTROLLER="$ROOT/build/clang-libcxx/libdavinci_av1_session_controller.so.0.1.0"
GDB_SCRIPT="$ROOT/installer/gdb/resolve_codec_session.gdb"
MANIFEST="$ROOT/build/clang-libcxx/artifact-manifest.txt"

[[ -x "$RESOLVE_BIN" ]] || {
    printf 'Error: Resolve executable missing: %s\n' "$RESOLVE_BIN" >&2
    exit 1
}
command -v "$GDB_BIN" >/dev/null 2>&1 || {
    printf 'Error: GDB not found: %s\n' "$GDB_BIN" >&2
    exit 1
}
[[ -r "$CONTROLLER" && -r "$GDB_SCRIPT" && -r "$MANIFEST" ]] || {
    printf '%s\n' 'Error: build artifacts, manifest or GDB session script are missing. Run scripts/build.sh first.' >&2
    exit 1
}
EXPECTED_CONTROLLER_SHA256="$(awk -F= '$1 == "controller_sha256" {print $2}' "$MANIFEST")"
[[ -n "$EXPECTED_CONTROLLER_SHA256" ]] || {
    printf '%s\n' 'Error: controller hash is missing from the build artifact manifest.' >&2
    exit 1
}
actual_hash="$(sha256sum "$CONTROLLER" | awk '{print $1}')"
[[ "$actual_hash" == "$EXPECTED_CONTROLLER_SHA256" ]] || {
    printf 'Error: session-controller SHA-256 mismatch: %s\n' "$actual_hash" >&2
    exit 1
}

if pgrep -f "^${RESOLVE_BIN//\//\\/}( |$)" >/dev/null 2>&1; then
    printf '%s\n' 'Error: a Resolve process is already running; refusing to create a second session.' >&2
    exit 73
fi

mkdir -p "$ROOT/reports"
cd "$ROOT"
printf 'Starting persistent codec-runtime Resolve session: %s\n' "$RESOLVE_BIN"
printf 'GDB session script: %s\n' "$GDB_SCRIPT"
printf 'Session diagnostic log: %s\n' "$ROOT/reports/resolve-session.log"
exec "$GDB_BIN" -q -nx -batch -x "$GDB_SCRIPT" --args "$RESOLVE_BIN" "$@"
