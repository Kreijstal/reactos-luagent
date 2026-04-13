# Debugging Guide

Debugging is a first-class part of this agent. The goal is to make failures
observable at the transport, protocol, Lua, transfer, process, and relay layers
without depending on a working remote shell.

## Logging

The agent supports structured logs with levels:

- `ERROR`
- `WARN`
- `INFO`
- `DEBUG`
- `TRACE`

Each log line includes:

- monotonic timestamp in milliseconds
- pid
- session id
- req id when relevant
- subsystem tag
- message

Example:

```text
320998379 INFO  pid=3349541 session=1 req=0 net    accepted
320998380 DEBUG pid=3349541 session=1 req=1 frame  RX type=HELLO len=21
320998380 DEBUG pid=3349541 session=1 req=1 frame  TX type=ACK len=23
```

## Environment Variables

### Log level

```bash
LUAGENT_LOG_LEVEL=INFO
LUAGENT_LOG_LEVEL=DEBUG
LUAGENT_LOG_LEVEL=TRACE
```

### Wire logging

```bash
LUAGENT_WIRE=off
LUAGENT_WIRE=summary
LUAGENT_WIRE=hex
```

Modes:

- `off`: no frame dump
- `summary`: log frame metadata only
- `hex`: summary plus hex dump of frame bytes

### File logging

```bash
LUAGENT_LOG_FILE=/tmp/luagent.log
```

Log rotation is automatic:

- rotate at `1 MiB`
- keep `luagent.log.1` through `luagent.log.3`

## Subsystem Tags

Current subsystem tags include:

- `net`
- `frame`
- `proto`
- `lua`
- `xfer`
- `proc`
- `relay`
- `timer`

Use these tags to narrow failures quickly.

## Recommended Development Setup

```bash
LUAGENT_LOG_LEVEL=DEBUG \
LUAGENT_WIRE=summary \
LUAGENT_LOG_FILE=/tmp/luagent.log \
./build/luagent 7000
```

For parser or framing bugs:

```bash
LUAGENT_LOG_LEVEL=TRACE \
LUAGENT_WIRE=hex \
LUAGENT_LOG_FILE=/tmp/luagent.log \
./build/luagent 7000
```

## Debug Ops

The following built-in ops expose live state:

- `debug.sessions`
- `debug.transfers`
- `debug.procs`
- `debug.stats`

Examples through the Linux client daemon:

```bash
python3 tools/client_daemon.py op debug.sessions
python3 tools/client_daemon.py op debug.transfers
python3 tools/client_daemon.py op debug.procs
python3 tools/client_daemon.py op debug.stats
```

### `debug.sessions`

Shows:

- current session id
- last RX/TX timestamps
- pending outbound bytes
- per-session frame counters
- closed state

### `debug.transfers`

Shows active upload/download state, including:

- req id
- path
- offset
- total bytes
- last progress time

### `debug.procs`

Shows current process execution state, including:

- proc id
- path
- start time
- last I/O time
- timeout configuration

### `debug.stats`

Shows global and current-session counters such as:

- frames received/sent
- bytes received/sent
- parse failures
- protocol errors
- Lua op failures
- transfer starts/completes/aborts
- process spawns/exits/timeouts
- session disconnects
- heartbeat events
- relay opens/closes

## Timeout Visibility

Timeouts are explicitly logged.

Examples:

```text
WARN  ... xfer  idle_timeout elapsed_ms=22001 threshold_ms=20000 action=abort
WARN  ... proc  timeout elapsed_ms=801 threshold_ms=800 action=kill
WARN  ... timer session idle timeout elapsed_ms=15010 threshold_ms=15000 action=close
```

This is the main way to distinguish real timeout behavior from random disconnects.

## Lua Error Visibility

Lua dispatch is protected. A bad Lua op should:

- log the failure under `lua`
- increment Lua error counters
- return a protocol `ERROR`
- keep the server alive

If you see a failing op, check:

- agent log output
- `debug.stats`
- `debug.sessions`

## Integration Tests

Native automated coverage is available through:

```bash
ctest --test-dir build --output-on-failure
```

Current automated checks include:

- HELLO / ACK
- PING / PONG
- malformed frame close
- partial-frame buffering
- list op
- upload and download
- transfer disconnect cleanup
- relay open/forward/close
- process spawn/kill/timeout
- `tool.run`
- `debug.stats`
- Linux client daemon workflow

## Practical Workflow

When debugging a new feature:

1. enable `DEBUG` logs and wire summary
2. reproduce through the integration harness or client daemon
3. inspect `debug.stats` and the subsystem-specific debug op
4. only switch to `hex` mode if you suspect framing or payload corruption

For ReactOS-specific bugs, keep the same log settings and copy the log file
back with the agent if console output is unreliable.
