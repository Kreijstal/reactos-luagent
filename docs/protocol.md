# Protocol Specification

## Goals

The v1 protocol is intentionally small:

- one TCP connection per client
- fixed binary frame header
- request/response correlation by request id
- compact text control payloads
- raw binary chunk payloads
- bounded buffering and explicit timeout handling

Security and compatibility with SSH are out of scope.

## Frame Header

All multi-byte integers are little-endian for v1.

| Field | Size | Notes |
| --- | ---: | --- |
| `magic` | 4 bytes | ASCII `RXSH` |
| `type` | 1 byte | message type |
| `flags` | 1 byte | reserved for future use |
| `req_id` | 4 bytes | request correlation id |
| `length` | 4 bytes | payload length in bytes |

Header size: 14 bytes.

Hard rules:

- `magic` must equal `RXSH`
- `length` must be `<= MAX_FRAME_PAYLOAD`
- unknown message types are rejected with `ERROR`
- partial frames are buffered until complete
- oversized or malformed frames terminate the session

Recommended initial limits:

- `MAX_FRAME_PAYLOAD = 65536`
- `MAX_CONTROL_PAYLOAD = 8192`
- `MAX_WRITE_QUEUE_BYTES = 262144`

## Message Types

| Value | Name | Purpose |
| ---: | --- | --- |
| 1 | `HELLO` | client hello and capability banner |
| 2 | `ACK` | generic success acknowledgement |
| 3 | `ERROR` | generic error response |
| 10 | `OP` | built-in operation request |
| 11 | `OP_RESULT` | built-in operation result |
| 20 | `PUT_BEGIN` | start upload |
| 21 | `PUT_CHUNK` | upload data chunk |
| 22 | `PUT_END` | complete upload |
| 30 | `GET_BEGIN` | start download |
| 31 | `GET_CHUNK` | download data chunk |
| 32 | `GET_END` | complete download |
| 40 | `PROC_SPAWN` | spawn executable |
| 41 | `STDIN` | process stdin data |
| 42 | `STDOUT` | process stdout data |
| 43 | `STDERR` | process stderr data |
| 44 | `EXIT` | process exit notification |
| 45 | `KILL` | client kill request |
| 50 | `PING` | heartbeat ping |
| 51 | `PONG` | heartbeat reply |

## Control Payload Encoding

Control frames use UTF-8 text with newline-delimited `key=value` records.
Binary chunk frames use raw bytes and may start with a compact binary prefix if
offset metadata is needed.

Example `OP`:

```text
name=list
path=C:\ReactOS
```

Example `OP_RESULT`:

```text
status=ok
count=3
```

Rules:

- each line contains exactly one `=`
- keys are ASCII identifiers
- values are raw text after the first `=`
- duplicate keys are invalid unless explicitly documented
- missing required keys produce `ERROR`

This format is intentionally unsophisticated. The point is deterministic parsing,
not expressiveness.

## Standard Control Keys

Common request keys:

- `name`
- `path`
- `src`
- `dst`
- `offset`
- `size`
- `mode`
- `timeout_ms`
- `idle_timeout_ms`

Common response keys:

- `status`
- `code`
- `message`
- `transfer_id`
- `proc_id`
- `bytes`
- `exit_code`
- `reason`

## Built-in Operations

v1 built-ins:

- `pwd`
- `list`
- `stat`
- `read`
- `write`
- `mkdir`
- `remove`
- `move`

The client should prefer `PUT_*` and `GET_*` for large file transfer. `read` and
`write` are for small bounded operations.

## Upload Flow

`PUT_BEGIN` payload:

```text
path=C:\temp\file.bin
size=12345
overwrite=0
chunk_size=4096
```

Server behavior:

- create temp file
- allocate transfer state
- reply with `ACK` including `transfer_id`

`PUT_CHUNK`:

- correlated by `req_id`
- payload is raw bytes
- server appends or writes at current offset

`PUT_END`:

- validates received size
- atomically finalizes temp file
- responds with `ACK` or `ERROR`

## Download Flow

`GET_BEGIN` payload:

```text
path=C:\temp\file.bin
chunk_size=4096
```

Server behavior:

- opens file
- replies with `ACK`
- emits one or more `GET_CHUNK`
- finishes with `GET_END`

## Process Flow

`PROC_SPAWN` payload:

```text
path=C:\ReactOS\system32\ipconfig.exe
argv0=ipconfig.exe
argv1=/all
timeout_ms=60000
idle_timeout_ms=10000
```

Rules:

- no shell parsing
- executable path is explicit
- arguments are ordered fields
- output is streamed via `STDOUT` and `STDERR`
- completion is reported by `EXIT`

## Error Codes

Initial stable error codes:

- `bad_magic`
- `bad_length`
- `bad_type`
- `bad_payload`
- `unknown_op`
- `not_found`
- `already_exists`
- `access_denied`
- `io_error`
- `timeout`
- `idle_timeout`
- `cancelled`
- `spawn_failed`
- `internal`

`ERROR` payload example:

```text
code=not_found
message=path does not exist
```

## Timeout Model

Session:

- send `PING` after 5 seconds of idle outbound silence
- close session after 15 seconds without inbound traffic

Transfer:

- abort if no progress for 20 seconds

Process:

- absolute timeout from spawn time
- idle timeout based on stdout, stderr, or stdin activity

These timers are independent and must not share a single overloaded state flag.
