#!/usr/bin/env bash

RESOLVE_BIN="${RESOLVE_BIN:-/opt/resolve/bin/resolve}"

if [[ ! -x "$RESOLVE_BIN" ]]; then
    printf 'Error: Resolve executable not found or not executable: %s\n' "$RESOLVE_BIN" >&2
    exit 1
fi

exec "$RESOLVE_BIN" "$@"
