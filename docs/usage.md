# Usage Guide

This project is meant to be operated from a Linux host talking to a barebones
ReactOS or Wine target. The intended workflow is:

1. start `luagent` on the target
2. connect to it from Linux using the client daemon
3. use file transfer, `tool.run`, debug ops, and port relay ops instead of a shell

## Start The Agent

Run the server on the target:

```bash
./build/luagent 7000
```

Useful diagnostics on the target:

```bash
LUAGENT_LOG_LEVEL=DEBUG \
LUAGENT_WIRE=summary \
LUAGENT_LOG_FILE=/tmp/luagent.log \
./build/luagent 7000
```

Environment knobs:

- `LUAGENT_LOG_LEVEL=ERROR|WARN|INFO|DEBUG|TRACE`
- `LUAGENT_WIRE=off|summary|hex`
- `LUAGENT_LOG_FILE=/path/to/agent.log`

## Start The Linux Client Daemon

The Linux-side helper is [tools/client_daemon.py](/home/kreijstal/git/reactos-luagent/tools/client_daemon.py).
It keeps a persistent TCP connection to the agent and exposes a local Unix
socket control path.

Foreground:

```bash
python3 tools/client_daemon.py start --agent-host 127.0.0.1 --agent-port 7000
```

Background:

```bash
python3 tools/client_daemon.py start \
  --agent-host 127.0.0.1 \
  --agent-port 7000 \
  --daemonize
```

Stop it:

```bash
python3 tools/client_daemon.py stop
```

## Common Commands

### Agent status

```bash
python3 tools/client_daemon.py status
```

This returns the current `debug.stats` snapshot from the agent.

### Run a built-in op

```bash
python3 tools/client_daemon.py op list path=.
python3 tools/client_daemon.py op debug.sessions
python3 tools/client_daemon.py op debug.transfers
python3 tools/client_daemon.py op debug.procs
python3 tools/client_daemon.py op debug.stats
```

### Upload and download files

```bash
python3 tools/client_daemon.py put ./local.bin C:\\temp\\remote.bin
python3 tools/client_daemon.py get C:\\temp\\remote.bin ./copy.bin
```

On POSIX-hosted Wine or Linux smoke targets, plain `/tmp/...` paths also work.

### Run a process through `tool.run`

`tool.run` is a Lua convenience op that launches a process using the existing
native process subsystem.

Raw op form:

```bash
python3 tools/client_daemon.py op tool.run \
  path=/usr/bin/python3 \
  argv0=/usr/bin/python3 \
  argv1=-c \
  "argv2=print('hello from tool.run')" \
  timeout_ms=5000 \
  idle_timeout_ms=5000
```

This returns an `OP_RESULT` containing at least:

- `status=ok`
- `proc_id=<id>`
- `path=<path>`

If you prefer direct process control commands instead of the Lua op:

```bash
python3 tools/client_daemon.py spawn /usr/bin/python3 -c "print('hello')"
python3 tools/client_daemon.py wait-proc 1
python3 tools/client_daemon.py kill 1
```

### Open and close a TCP relay

Port relay is exposed as Lua ops and is useful for workflows like `gdbserver`.

Open a relay from a local listener on the target to some target host/port:

```bash
python3 tools/client_daemon.py op port_open \
  listen_host=127.0.0.1 \
  listen_port=0 \
  target_host=127.0.0.1 \
  target_port=44583
```

This returns:

- `status=ok`
- `relay_id=<id>`
- `listen_port=<actual_bound_port>`

Close it:

```bash
python3 tools/client_daemon.py op port_close relay_id=1
```

List session-owned relays:

```bash
python3 tools/client_daemon.py op port_list
```

## Debugging Workflow

Recommended debug ops:

- `debug.sessions`
- `debug.transfers`
- `debug.procs`
- `debug.stats`

Examples:

```bash
python3 tools/client_daemon.py op debug.sessions
python3 tools/client_daemon.py op debug.transfers
python3 tools/client_daemon.py op debug.procs
python3 tools/client_daemon.py op debug.stats
```

Recommended log setup while developing:

```bash
LUAGENT_LOG_LEVEL=DEBUG LUAGENT_WIRE=summary LUAGENT_LOG_FILE=/tmp/luagent.log ./build/luagent 7000
```

## Typical Workflow For Cross-Compiled Tools

A practical ReactOS workflow is:

1. cross-compile your tool on Linux
2. upload it with `put`
3. launch it with `tool.run` or `spawn`
4. inspect output with `wait-proc`
5. if needed, open a relay with `port_open`

Example:

```bash
python3 tools/client_daemon.py put ./gdbserver.exe C:\\temp\\gdbserver.exe
python3 tools/client_daemon.py op port_open listen_host=0.0.0.0 listen_port=0 target_host=127.0.0.1 target_port=9000
python3 tools/client_daemon.py op tool.run \
  path=C:\\temp\\gdbserver.exe \
  argv0=C:\\temp\\gdbserver.exe \
  argv1=:9000 \
  argv2=C:\\temp\\target.exe \
  timeout_ms=0 \
  idle_timeout_ms=0
```

That avoids dependence on `cmd.exe` while still giving you launch, relay, and
debugging primitives.
