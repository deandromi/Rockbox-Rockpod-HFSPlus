#!/bin/sh
# Store a durable semantic project note.
# Usage: tools/xerj/remember.sh "text" [kind]
set -eu
TEXT=${1-}
KIND=${2:-note}
[ -n "$TEXT" ] || { echo "usage: $0 \"text\" [kind]" >&2; exit 2; }

python3 - "$TEXT" "$KIND" <<'PY'
import json
import sys
import urllib.request

body = json.dumps({
    "text": sys.argv[1],
    "metadata": {"kind": sys.argv[2]},
}).encode("utf-8")
request = urllib.request.Request(
    "http://127.0.0.1:9200/_memory/rockpod-project",
    data=body,
    headers={"content-type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request) as response:
    print(json.load(response)["id"])
PY
