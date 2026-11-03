#!/bin/sh
# Recall durable semantic project notes.
# Usage: tools/xerj/recall.sh "question" [k]
set -eu
QUERY=${1-}
K=${2:-5}
[ -n "$QUERY" ] || { echo "usage: $0 \"question\" [k]" >&2; exit 2; }

python3 - "$QUERY" "$K" <<'PY'
import json
import sys
import urllib.request

body = json.dumps({
    "query": sys.argv[1],
    "semantic": True,
    "k": int(sys.argv[2]),
}).encode("utf-8")
request = urllib.request.Request(
    "http://127.0.0.1:9200/_memory/hfsplus-rockboxed-ipod-project/_recall",
    data=body,
    headers={"content-type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request) as response:
    hits = json.load(response).get("hits", [])
for hit in hits:
    print(f"{hit.get('score', 0):.3f}  {hit.get('text', '')}")
PY
