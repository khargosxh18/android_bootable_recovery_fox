# Fox RPC schema — the op/args contract

**Single source of truth** for the Fox RPC wire shape, shared by the two ends:

- **engine** — `bootable/recovery/fox_fifo/fox_fifo.cpp` reads these fields via
  the `arg_*` helpers in `Cmd_*` handlers (`Fox_Fifo::Run_Command` dispatches on
  `op`).
- **client** — `external/foxcli` builds them in `Command::to_request()`
  (`src/args.rs`). The raw `fox fifo send <tokens>` / `fox rpc` paths bypass the
  typed builder.

A request is `{"v":1, "id":"<opt>", "op":"<op>", "args":{…}}`. Responses are an
NDJSON event stream (`log`/`progress`/`data`/`error`, terminated by `result`).
**When you change a field here, change both ends and this table together.**

## Per-op fields

| op | args |
|---|---|
| `status`, `sideload`, `reflash`, `log`, `storages` | — |
| `mount`, `unmount` (`umount`) | `paths`: string[] |
| `flash` (`install`) | `zips`: string[]; `verify`: bool?; `reflash`: bool?; `unmount_system`: bool; `unmount_vendor`: bool |
| `backup` | `parts`: string[]; `name`; `path`; `storage`; `compress`: bool; `digest`: bool (default true) |
| `restore` | `parts`: string[]; `name`; `path`; `digest_check`: bool (default true) |
| `decrypt` | `password`; `user`: int |
| `wipe` | `parts`: string[] |
| `format_data` | `confirm`: bool |
| `reboot` | `target` |
| `partition` | `path`; `action`; `fs` |
| `addons` | `action`; `names`: string[] |
| `internal` | `action`; `name`; `value`; `page` |
| `mtp` | `action` |
| `input` | `action`; `key`; `code`: int; `x`,`y`,`x2`,`y2`,`duration`: int |
| `screencap` | `path`; `base64`: bool |
| `screenstream` | `action`; `fps`: int |
| `ors` | `path` |
| `ors-cmd` | `raw`: string[] |

Notes:
- `input`/`screencap`(base64)/`screenstream` are intercepted by
  `Fox_Command_Dispatcher::dispatch_control` and consumed typed by
  `Fox_Remote_Input` / `Fox_Screen_Stream`; they do not reach `Run_Command`.
