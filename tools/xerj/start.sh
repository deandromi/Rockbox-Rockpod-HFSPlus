#!/bin/sh
# Start the local XERJ node for this project.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
DATA=${XERJ_PROJECT_DATA:-"$ROOT/.xerj-data"}
BIN=${XERJ_BIN:-"$HOME/.local/bin/xerj"}

if [ ! -x "$BIN" ]; then
    echo "XERJ not found at $BIN; install it or set XERJ_BIN." >&2
    exit 1
fi
if curl -fsS http://127.0.0.1:9200/ >/dev/null 2>&1; then
    echo "XERJ already running on 127.0.0.1:9200."
    exit 0
fi

mkdir -p "$DATA"
XERJ_DISABLE_FEEDBACK=true nohup "$BIN" \
    --insecure --embed-mode neural --data-dir "$DATA" \
    >"$DATA/server.log" 2>&1 &
echo $! > "$DATA/server.pid"

for i in $(seq 1 120); do
    if curl -fsS http://127.0.0.1:9200/ >/dev/null 2>&1; then
        echo "XERJ ready after ${i}s: http://127.0.0.1:9200"
        exit 0
    fi
    sleep 1
done
echo "XERJ did not become ready; see $DATA/server.log" >&2
exit 1
