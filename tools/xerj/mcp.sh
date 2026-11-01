#!/bin/sh
# MCP stdio wrapper for OpenCode and other agent sessions.
# Proxies to an already-running XERJ node; it does not start one.
set -eu
BIN=${XERJ_BIN:-"$HOME/.local/bin/xerj"}
export XERJ_URL=${XERJ_URL:-http://127.0.0.1:9200}
export XERJ_DISABLE_FEEDBACK=true
exec "$BIN" mcp --disable-feedback
