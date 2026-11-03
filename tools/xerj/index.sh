#!/bin/sh
# Reindex the project-specific docs and HFS+ code.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN=${XERJ_BIN:-"$HOME/.local/bin/xerj"}

if ! curl -fsS http://127.0.0.1:9200/ >/dev/null 2>&1; then
    echo "XERJ is not running; start tools/xerj/start.sh first." >&2
    exit 1
fi
exec "$BIN" autoindex "$ROOT" --prefix hfsplus-rockboxed-ipod --workers 4
