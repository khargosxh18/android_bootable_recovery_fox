# Fox RPC v1

Scope: the modern `fox` command engine in `fox_fifo/`. It is always built and is
the common application protocol for local FIFO/WebUSB/USB video streaming clients.

The **op/args field contract** (which op accepts which JSON fields) is the single
shared schema in [`fox-rpc-schema.md`](fox-rpc-schema.md) — change both ends and
that table together.

## Transport

FIFO paths are defined in `orscmd/orscmd.h`:

- `FOX_INPUT_FILE` = `/system/bin/foxin`
- `FOX_OUTPUT_FILE` = `/system/bin/foxout`
- `FOX_CANCEL_FILE` = `/system/bin/foxcancel`
- `FOX_SCREEN_STREAM_FILE` = `/system/bin/foxscreenout`

One request is written to `foxin` as a UTF-8 JSON object, then the writer closes
the FIFO:

```json
{"v":1,"id":"optional-client-id","op":"status","args":{}}
```

`foxout` returns newline-delimited JSON events. The final event is always
`result`:

```json
{"event":"log","text":"Mounting /data\n"}
{"event":"progress","phase":"overall","percent":42,"label":"Data","current_bytes":104857600,"total_bytes":524288000,"bytes_per_second":20971520,"eta_seconds":20}
{"event":"result","code":0}
```


## Request

- `v`: protocol version. Must be `1`.
- `id`: optional client request id, echoed on response events when present.
- `op`: operation name, such as `status`, `backup`, `input`, or `screencap`.
- `args`: operation-specific object. Missing `args` is treated as `{}`.

The protocol is intentionally not a CLI. There are no shell flags, aliases,
usage text, or sentinel bytes on the wire. Human command parsing belongs in
`external/foxcli`; recovery receives structured requests.

## Events

- `log`: ordinary recovery output, `{ "event": "log", "text": "..." }`.
- `data`: machine data, `{ "event": "data", "name": "...", "value": ... }`.
- `progress`: progress updates, `{ "event": "progress", "phase": "overall", "percent": 42 }`.
- `error`: structured error, `{ "event": "error", "code": "...", "message": "..." }`.
- `result`: final result, `{ "event": "result", "code": 0 }`.

Progress has two phases in v1: `overall` for the whole operation and `item` for
the current partition/file. Backup/restore progress events may also include
flat optional fields: `label`, `current_bytes`, `total_bytes`,
`bytes_per_second`, `eta_seconds`, `current_files`, `total_files`, `size_text`,
and `file_text`. Clients should ignore fields they do not need.

### Structured results via `data` events

Query/list handlers return their result as a `data` event rather than a scraped
text block. The engine helper is `Fox_Command_Dispatcher::EmitData(name,
payload)` (it writes a `data` event on the active transport, or prints the JSON
to the console when GUI-invoked). Current named payloads:

- `status` — `fox status`: a flat object (`release`, `device_model`,
  `storage_encrypted` (bool), …). foxcli deserializes it into `StatusSnapshot`.
- `list` — listing forms (`mount`/`backup`/`wipe`/`partition`/`addons`/
  `storages` with no target): `{ "items": [{ mount_point, display_name,
  mounted (bool), extra }] }`. foxcli renders it via `partition_rows_from_data`.
- `screencap` — base64 PNG (streamed to stdout by the client).

Every status/list handler now emits a `data` event — there are no more
`FOX_*_BEGIN/END` text blocks. Object payloads (one `data` event each):

- `mtp` — `{ enabled }`
- `screenstream` — `{ running, path, fps, [format] }`

Array payloads use `{ "items": [...] }`: `list` (partition/storage/addon

Booleans are real JSON booleans; the `auth` field is either `"remote-password"`
or a `token` field carries the random per-boot token (a configured password is
never echoed). This is the structured surface a dashboard consumes directly.

## Flow

1. `Fox_Channel::Setup()` creates the FIFOs.
2. The GUI event loop watches `Fox_Channel::InputFd()` and `CancelFd()`.
3. `Fox_Channel::HandleInput()` parses the JSON request and dispatches it.
4. `Fox_Command_Dispatcher` serializes execution and wraps `gui_print` as
   `log` events.
5. `GUIAction::foxcmd` runs `Fox_Fifo::Run_Command()` on the action thread.
6. Completion writes the final `result` event and closes `foxout`.

Only one command runs at a time. The dashboard job queue preserves that model
because recovery mutates global state such as `PartitionManager` and
`DataManager`.

High-frequency input/screen commands can still bypass the single-action page
where appropriate, but they use the same request and event framing.

## Related

- `gui/AGENTS.md` - the `foxcmd` GUI action.
