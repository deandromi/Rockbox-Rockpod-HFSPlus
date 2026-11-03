# Local XERJ semantic memory

This is an optional local semantic search/index layer for the project. It uses
the XERJ Community binary in neural embedding mode and stores its data in
`.xerj-data/`, which is git-ignored.

It is deliberately scoped by `.xerjignore` to:

- project docs and root markdown;
- `firmware/hfsplus/`;
- `tools/hfsplus/`;
- `tools/rockpod/`.

It does not index the inherited Rockbox tree, build directories, iPod backups,
`.rockpod-rw` or secrets.

## Start and index

```sh
tools/xerj/start.sh
tools/xerj/index.sh
```

`start.sh` runs the node on loopback only (`127.0.0.1:9200`) with
`XERJ_DISABLE_FEEDBACK=true`. It uses the neural embedder (~90 MB model,
roughly 1.5 GB RSS after indexing on the project test machine). Stop it with:

```sh
kill "$(cat .xerj-data/server.pid)"
```

## Search

```sh
~/.local/bin/xerj search --prefix rockpod \
  "where is overlay stage 11 validated and what does it check?"
```

The CLI search is lexical. Use `--json` for raw results. For exact definitions:

```sh
~/.local/bin/xerj def --prefix rockpod overlay_validate
```

## Semantic project memory

The durable memory namespace is `rockpod-project`:

```sh
tools/xerj/remember.sh \
  "V2.1.1 fixed legacy base_id entries without the BASE flag." root-cause

tools/xerj/recall.sh \
  "why was the overlay rejected at stage 11?"
```

This uses XERJ's `/_memory` semantic recall, so it works for decisions,
rationale and hardware notes that are not easily grep-able.

## MCP for future sessions

The node must be running first; `xerj mcp` proxies to it and does not start one.

```sh
opencode mcp add xerj \
  --env XERJ_URL=http://127.0.0.1:9200 \
  -- "$PWD/tools/xerj/mcp.sh"
opencode mcp list
```

That writes a project `opencode.json` with an absolute path, so the file is
machine-specific and git-ignored. By hand it looks like:

```json
{
  "mcp": {
    "servers": {
      "xerj": {
        "type": "local",
        "command": ["/absolute/path/to/hfsplus-rockboxed-ipod/tools/xerj/mcp.sh"],
        "environment": { "XERJ_URL": "http://127.0.0.1:9200" }
      }
    }
  }
}
```

OpenCode reloads MCP servers when the config is saved. The wrapper sets
`XERJ_DISABLE_FEEDBACK=true` so no field report is ever sent.

## Reindex after changes

```sh
tools/xerj/index.sh
```

The index is not committed. Rebuild it after changing docs or custom HFS+ code.
