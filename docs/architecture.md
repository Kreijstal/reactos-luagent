# System Architecture

## Overview

The agent is split into a small set of explicit modules. C owns transport,
resource lifetime, timers, file handles, process handles, and bounded memory.
Lua owns the operation registry and request semantics for built-in commands.

```text
Client
  |
  v
TCP listener (libuv)
  |
  v
frame parser / serializer
  |
  v
protocol router
  |
  +--> session manager
  |      |
  |      +--> heartbeat timers
  |      +--> request table
  |      +--> transfer table
  |      +--> process table
  |
  +--> control handlers in C
  |
  +--> Lua dispatcher
         |
         +--> filesystem bindings
         +--> process bindings
         +--> op registry
```

## Module List

### `src/server.c`

- owns the libuv loop entry points
- accepts clients
- reads TCP bytes
- feeds the frame parser
- owns outbound write queue

### `src/session.c`

- allocates per-client session state
- tracks last RX and TX activity
- tracks heartbeat timers
- owns request, transfer, and process registries

### `src/frame.c`

- encodes outbound frames
- incrementally decodes inbound bytes
- validates header and payload lengths

### `src/protocol.c`

- defines message handling policy
- parses control payloads
- dispatches to C or Lua handlers
- maps native failures to wire errors

### `src/transfer.c`

- manages upload and download state
- enforces chunk sequencing and progress
- handles safe temp-file writes

### `src/proc.c`

- owns child process state
- attaches stdio pipes
- streams stdout and stderr
- enforces absolute and idle timeouts

### `src/lua_engine.c`

- initializes Lua
- loads `lua/bootstrap.lua`
- exposes native bindings
- dispatches `OP` requests to registered Lua handlers

### `src/bindings.c`

- implements the Lua-visible `agent.*` API
- marshals values between C and Lua

### `src/util.c`

- small shared helpers
- bounded string and buffer helpers
- monotonic time wrappers

## Request Lifecycle

Built-in operation:

1. TCP bytes arrive in `server.c`.
2. `frame.c` reconstructs a complete frame.
3. `protocol.c` validates frame type and payload format.
4. The payload is parsed into a request map.
5. `lua_engine.c` looks up the Lua operation by `name`.
6. Lua calls filesystem or process bindings through `bindings.c`.
7. Result data is normalized into key/value response text.
8. `frame.c` serializes an `OP_RESULT`.
9. `server.c` queues the reply for write.

Upload:

1. Client sends `PUT_BEGIN`.
2. `transfer.c` creates transfer state and a temp file.
3. Client sends `PUT_CHUNK` frames.
4. `transfer.c` appends data and updates progress timestamps.
5. Timer checks abort stalled transfers.
6. Client sends `PUT_END`.
7. Temp file is finalized atomically when possible.
8. `ACK` or `ERROR` is returned.

Process:

1. Client sends `PROC_SPAWN`.
2. `proc.c` validates inputs and starts the child.
3. stdout and stderr pipes are attached.
4. Output is emitted as `STDOUT` and `STDERR`.
5. Timeout checks run independently.
6. On exit or kill, `EXIT` is sent.

## Failure Model

The first version should prefer explicit closure over heroic recovery.

- Bad frame header: close session.
- Oversized frame: close session.
- Unknown op: send `ERROR`, keep session alive.
- Transfer stall: abort that transfer, keep session alive.
- Process timeout: kill process, send `EXIT reason=timeout`.
- Lua handler failure: send `ERROR code=internal`, keep session alive.

## Timeout Model

Timeouts are separate state machines:

- session idle timeout
- session heartbeat timer
- transfer progress timeout
- process absolute timeout
- process idle timeout

Do not merge them into a single generic “deadline” field.

## Directory Layout

```text
.
├── CMakeLists.txt
├── README.md
├── docs/
│   ├── architecture.md
│   ├── protocol.md
│   └── test-plan.md
├── lua/
│   ├── bootstrap.lua
│   └── ops/
│       └── list.lua
├── src/
│   ├── bindings.c
│   ├── bindings.h
│   ├── frame.c
│   ├── frame.h
│   ├── lua_engine.c
│   ├── lua_engine.h
│   ├── main.c
│   ├── proc.c
│   ├── proc.h
│   ├── protocol.c
│   ├── protocol.h
│   ├── server.c
│   ├── server.h
│   ├── session.c
│   ├── session.h
│   ├── transfer.c
│   ├── transfer.h
│   ├── util.c
│   └── util.h
└── tests/
    └── host_client/
        └── client.c
```
