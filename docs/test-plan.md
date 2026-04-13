# Test Plan

## Strategy

Wine is the first integration environment. ReactOS remains the final deployment
target and must be used for smoke testing once transport and file operations are
stable.

The test priority is:

1. deterministic frame parsing and protocol correctness
2. bounded memory behavior under large transfers
3. timeout and dead-peer handling
4. process execution only after transport and file operations are stable

## Unit-Level Checks

- frame header encode/decode
- malformed header rejection
- payload length validation
- control payload parser validation
- key collision and missing-key handling

## Wine Integration Matrix

- connect and exchange `HELLO` / `ACK`
- `PING` / `PONG`
- session idle close
- unknown `OP` returns `ERROR`
- sample `list` operation reaches Lua and responds
- `PUT_BEGIN` / `PUT_CHUNK` / `PUT_END`
- `GET_BEGIN` / `GET_CHUNK` / `GET_END`
- transfer stall timeout
- disconnect during upload
- large file transfer without whole-file buffering

## ReactOS Smoke Matrix

- agent starts and listens
- directory listing on system drive
- file upload into a writable directory
- file download from a known test file
- process spawn for a simple executable
- stdout and stderr streaming sanity
- timeout kill sanity

## Hardening

- malformed frame fuzzing
- repeated connect/disconnect loops
- stalled peer tests
- hung child process tests
- write queue saturation tests
- leak and handle lifetime checks

## Exit Criteria For MVP

- stable transport in Wine
- stable file ops in Wine
- chunked transfer works without unbounded buffers
- ReactOS smoke tests pass for connect, list, upload, and download
- process execution is optional until platform behavior is proven
