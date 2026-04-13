# reactos-luagent

Minimal remote agent for ReactOS using C, libuv, and embedded Lua.

The first target is Wine for fast iteration. The deployment target is ReactOS.
The design avoids shell dependencies and browser-era tooling. v1 is a small,
custom framed protocol for deterministic built-in operations, file transfer,
heartbeat, and optional process spawning.

## Status

This repository currently contains:

- protocol and architecture docs for v1
- a buildable C skeleton with explicit module boundaries
- Lua bootstrap and a sample `list` operation
- a tiny host-side client stub for protocol bring-up
- the earlier `hello.c` prototype kept as a reference artifact

## Scope

Included in v1:

- TCP server
- binary frame header
- request ids
- HELLO / ACK / PING / PONG
- Lua-dispatched built-in ops
- file operations and chunked transfers
- absolute and idle timeouts
- optional process launch with streamed stdio

Excluded from v1:

- SSH compatibility
- authentication and crypto
- PTY emulation
- shell parsing
- port forwarding
- compression

## Build

Dependencies:

- CMake 3.16+
- libuv
- Lua 5.3 or 5.4

Example:

```bash
cmake -S . -B build
cmake --build build
```

## Docs

- [docs/protocol.md](/home/kreijstal/git/reactos-luagent/docs/protocol.md)
- [docs/architecture.md](/home/kreijstal/git/reactos-luagent/docs/architecture.md)
- [docs/test-plan.md](/home/kreijstal/git/reactos-luagent/docs/test-plan.md)
- [docs/usage.md](/home/kreijstal/git/reactos-luagent/docs/usage.md)
- [docs/debugging.md](/home/kreijstal/git/reactos-luagent/docs/debugging.md)

## Linux Client Daemon

For shell-friendly Linux use, there is a Python client daemon at
[tools/client_daemon.py](/home/kreijstal/git/reactos-luagent/tools/client_daemon.py).
It keeps a persistent TCP connection to the agent and exposes a local Unix
socket control interface through simple subcommands.

Example:

```bash
python3 tools/client_daemon.py start --agent-host 127.0.0.1 --agent-port 7000 --daemonize
python3 tools/client_daemon.py status
python3 tools/client_daemon.py op list path=.
python3 tools/client_daemon.py op tool.run path=/usr/bin/python3 argv0=/usr/bin/python3 argv1=-c "argv2=print('hello')"
python3 tools/client_daemon.py put ./local.bin C:\\temp\\remote.bin
python3 tools/client_daemon.py get C:\\temp\\remote.bin ./copy.bin
```

See [docs/usage.md](/home/kreijstal/git/reactos-luagent/docs/usage.md) for the
practical operator workflow, including `tool.run`, debug ops, and port relay
usage.
