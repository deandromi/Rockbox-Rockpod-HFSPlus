#!/bin/sh
# MCP stdio wrapper for future OpenCode/agent sessions.
set -eu
BIN=${XERJ_BIN:-"$HOME/.local/bin/xerj"}
export XERJ_URL=${XERJ_URL:-http://127.0.0.1:9200}
exec "$BIN" mcp
