#!/usr/bin/env python3
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import threading
from pathlib import Path

HOST = "127.0.0.1"
PORT = 7010


def frame(msg_type, req_id, payload=b""):
    return b"RXSH" + bytes([msg_type, 0]) + struct.pack("<II", req_id, len(payload)) + payload


def recv_exact(sock, n):
    out = b""
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise RuntimeError("short read")
        out += chunk
    return out


def recv_frame(sock):
    hdr = recv_exact(sock, 14)
    if hdr[:4] != b"RXSH":
        raise RuntimeError("bad magic")
    msg_type = hdr[4]
    req_id, length = struct.unpack("<II", hdr[6:14])
    payload = recv_exact(sock, length)
    return msg_type, req_id, payload


def wait_port(port):
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            with socket.create_connection((HOST, port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def kv(payload):
    text = payload.decode("utf-8", errors="replace")
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k] = v
    return out


def expect(cond, message):
    if not cond:
        raise AssertionError(message)


def with_server(server_path):
    proc = subprocess.Popen([server_path, str(PORT)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        wait_port(PORT)
        return proc
    except Exception:
        proc.kill()
        proc.wait(timeout=2)
        raise


def test_basic():
    with socket.create_connection((HOST, PORT), timeout=2) as s:
        s.sendall(frame(1, 1, b"client=test\nversion=1"))
        t, req, payload = recv_frame(s)
        expect((t, req) == (2, 1), "HELLO/ACK failed")
        expect(kv(payload)["status"] == "ok", "bad hello ack")

        s.sendall(frame(50, 2, b""))
        t, req, payload = recv_frame(s)
        expect((t, req) == (51, 2), "PING/PONG failed")

        s.sendall(frame(10, 3, b"name=list\npath=."))
        t, req, payload = recv_frame(s)
        expect((t, req) == (11, 3), "OP list failed")
        expect(kv(payload)["status"] == "ok", "list not ok")

        s.sendall(frame(10, 4, b"name=nope"))
        t, req, payload = recv_frame(s)
        expect((t, req) == (3, 4), "unknown op should error")
        expect(kv(payload)["code"] == "unknown_op", "wrong unknown op error")

        s.sendall(frame(10, 5, b"name=debug.stats"))
        t, req, payload = recv_frame(s)
        expect((t, req) == (11, 5), "debug.stats failed")
        info = kv(payload)
        expect(info["status"] == "ok", "debug.stats not ok")
        expect("global.frames_rx" in info, "debug.stats missing counters")


def test_unknown_type_and_partial():
    with socket.create_connection((HOST, PORT), timeout=2) as s:
        s.sendall(frame(99, 5, b""))
        t, req, payload = recv_frame(s)
        expect((t, req) == (3, 5), "unknown type should error")
        expect(kv(payload)["code"] == "bad_type", "wrong bad_type code")

    with socket.create_connection((HOST, PORT), timeout=2) as s:
        pkt = frame(1, 6, b"")
        s.sendall(pkt[:5])
        time.sleep(0.1)
        s.sendall(pkt[5:])
        t, req, _ = recv_frame(s)
        expect((t, req) == (2, 6), "partial frame buffering failed")

    with socket.create_connection((HOST, PORT), timeout=2) as s:
        s.sendall(b"NOPE" + b"\x01\x00" + struct.pack("<II", 7, 0))
        time.sleep(0.2)
        try:
            data = s.recv(1)
        except OSError:
            data = b""
        expect(data == b"", "malformed frame should close session")


def upload(sock, req_id, local_bytes, remote_path):
    payload = f"path={remote_path}\nsize={len(local_bytes)}\noverwrite=1".encode()
    sock.sendall(frame(20, req_id, payload))
    t, req, payload = recv_frame(sock)
    expect((t, req) == (2, req_id), "PUT_BEGIN ack failed")
    chunk_size = 7
    off = 0
    while off < len(local_bytes):
        chunk = local_bytes[off:off + chunk_size]
        sock.sendall(frame(21, req_id, chunk))
        off += len(chunk)
    sock.sendall(frame(22, req_id, b""))
    t, req, payload = recv_frame(sock)
    expect((t, req) == (2, req_id), "PUT_END ack failed")
    expect(kv(payload)["bytes"] == str(len(local_bytes)), "wrong upload size ack")


def download(sock, req_id, remote_path):
    payload = f"path={remote_path}\nchunk_size=5".encode()
    sock.sendall(frame(30, req_id, payload))
    t, req, payload = recv_frame(sock)
    expect((t, req) == (2, req_id), "GET_BEGIN ack failed")
    info = kv(payload)
    total = int(info["size"])
    received = b""
    while True:
        t, req, payload = recv_frame(sock)
        expect(req == req_id, "download req mismatch")
        if t == 31:
            received += payload
        elif t == 32:
            break
        else:
            raise AssertionError(f"unexpected download frame type {t}")
    expect(len(received) == total, "download size mismatch")
    return received


def test_transfer():
    source = b"alpha\nbeta\ngamma\n1234567890\n"
    remote = f"{tempfile.gettempdir()}/luagent-transfer-test.txt"
    part = remote + ".part"
    for path in (remote, part):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass

    with socket.create_connection((HOST, PORT), timeout=2) as s:
        upload(s, 100, source, remote)
        expect(Path(remote).read_bytes() == source, "uploaded file mismatch")
        expect(download(s, 101, remote) == source, "downloaded file mismatch")

    remote2 = f"{tempfile.gettempdir()}/luagent-transfer-abort.txt"
    part2 = remote2 + ".part"
    for path in (remote2, part2):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    with socket.create_connection((HOST, PORT), timeout=2) as s:
        payload = f"path={remote2}\nsize=20\noverwrite=1".encode()
        s.sendall(frame(20, 102, payload))
        recv_frame(s)
        s.sendall(frame(21, 102, b"partial"))
    time.sleep(0.5)
    expect(not Path(part2).exists(), "temp file should be removed after disconnect")


class EchoServer:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind((HOST, 0))
        self.sock.listen(5)
        self.port = self.sock.getsockname()[1]
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        self.sock.settimeout(0.2)
        while not self.stop.is_set():
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with conn:
                while True:
                    data = conn.recv(4096)
                    if not data:
                        break
                    conn.sendall(data)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, exc_type, exc, tb):
        self.stop.set()
        try:
            self.sock.close()
        except OSError:
            pass
        self.thread.join(timeout=1)


def test_port_forward():
    with EchoServer() as echo:
        with socket.create_connection((HOST, PORT), timeout=2) as s:
            payload = (
                f"name=port_open\nlisten_host=127.0.0.1\nlisten_port=0\n"
                f"target_host=127.0.0.1\ntarget_port={echo.port}"
            ).encode()
            s.sendall(frame(10, 300, payload))
            t, req, payload = recv_frame(s)
            expect((t, req) == (11, 300), "port_open op failed")
            info = kv(payload)
            relay_id = int(info["relay_id"])
            relay_port = int(info["listen_port"])

            with socket.create_connection((HOST, relay_port), timeout=2) as relay_sock:
                relay_sock.sendall(b"relay-test")
                echoed = recv_exact(relay_sock, len(b"relay-test"))
                expect(echoed == b"relay-test", "relay data mismatch")

            payload = f"name=port_close\nrelay_id={relay_id}".encode()
            s.sendall(frame(10, 301, payload))
            t, req, payload = recv_frame(s)
            expect((t, req) == (11, 301), "port_close op failed")

            time.sleep(0.2)
            failed = False
            try:
                with socket.create_connection((HOST, relay_port), timeout=1):
                    pass
            except OSError:
                failed = True
            expect(failed, "relay port should be closed")


def recv_until_exit(sock, proc_id, timeout=5):
    deadline = time.time() + timeout
    frames = []
    while time.time() < deadline:
        sock.settimeout(max(0.1, deadline - time.time()))
        t, req, payload = recv_frame(sock)
        frames.append((t, req, payload))
        if t == 44 and req == proc_id:
            return frames
    raise RuntimeError("did not receive EXIT")


def test_process():
    py = sys.executable
    with socket.create_connection((HOST, PORT), timeout=2) as s:
        payload = (
            f"path={py}\nargv0={py}\nargv1=-c\n"
            "argv2=import sys; sys.stdout.write('out'); sys.stderr.write('err')\n"
            "timeout_ms=5000\nidle_timeout_ms=5000"
        ).encode()
        s.sendall(frame(40, 200, payload))
        t, req, payload = recv_frame(s)
        expect((t, req) == (2, 200), "spawn ack failed")
        proc_id = int(kv(payload)["proc_id"])
        frames = recv_until_exit(s, proc_id)
        stdout = b"".join(p for t, r, p in frames if t == 42)
        stderr = b"".join(p for t, r, p in frames if t == 43)
        exit_info = kv([p for t, r, p in frames if t == 44][0])
        expect(stdout == b"out", "stdout mismatch")
        expect(stderr == b"err", "stderr mismatch")
        expect(exit_info["reason"] == "exit", "normal exit reason mismatch")

    with socket.create_connection((HOST, PORT), timeout=2) as s:
        payload = (
            f"path={py}\nargv0={py}\nargv1=-c\nargv2=import time; time.sleep(30)\n"
            "timeout_ms=10000\nidle_timeout_ms=10000"
        ).encode()
        s.sendall(frame(40, 201, payload))
        t, req, payload = recv_frame(s)
        proc_id = int(kv(payload)["proc_id"])
        s.sendall(frame(45, 202, f"proc_id={proc_id}".encode()))
        t, req, payload = recv_frame(s)
        expect((t, req) == (2, 202), "kill ack failed")
        frames = recv_until_exit(s, proc_id)
        exit_info = kv([p for t, r, p in frames if t == 44][0])
        expect(exit_info["reason"] == "killed", "kill reason mismatch")

    with socket.create_connection((HOST, PORT), timeout=2) as s:
        payload = (
            f"path={py}\nargv0={py}\nargv1=-c\nargv2=import time; time.sleep(3)\n"
            "timeout_ms=800\nidle_timeout_ms=5000"
        ).encode()
        s.sendall(frame(40, 203, payload))
        t, req, payload = recv_frame(s)
        proc_id = int(kv(payload)["proc_id"])
        frames = recv_until_exit(s, proc_id)
        exit_info = kv([p for t, r, p in frames if t == 44][0])
        expect(exit_info["reason"] == "timeout", "timeout reason mismatch")


def test_tool_run():
    py = sys.executable
    with socket.create_connection((HOST, PORT), timeout=2) as s:
        payload = (
            f"name=tool.run\npath={py}\nargv0={py}\nargv1=-c\n"
            "argv2=print('toolrun')\ntimeout_ms=5000\nidle_timeout_ms=5000"
        ).encode()
        s.sendall(frame(10, 210, payload))
        t, req, payload = recv_frame(s)
        expect((t, req) == (11, 210), "tool.run op failed")
        info = kv(payload)
        expect(info["status"] == "ok", "tool.run not ok")
        proc_id = int(info["proc_id"])
        frames = recv_until_exit(s, proc_id)
        stdout = b"".join(p for t, r, p in frames if t == 42)
        exit_info = kv([p for t, r, p in frames if t == 44][0])
        expect(stdout.replace(b"\r\n", b"\n") == b"toolrun\n", "tool.run stdout mismatch")
        expect(exit_info["reason"] == "exit", "tool.run exit reason mismatch")


def test_heartbeat():
    with socket.create_connection((HOST, PORT), timeout=20) as s:
        s.settimeout(8)
        t, req, payload = recv_frame(s)
        expect(t == 50, "expected heartbeat ping")
        expect(kv(payload)["status"] == "ping", "heartbeat payload mismatch")


def main():
    server_path = sys.argv[1]
    proc = with_server(server_path)
    try:
        test_basic()
        test_unknown_type_and_partial()
        test_transfer()
        test_port_forward()
        test_process()
        test_tool_run()
        test_heartbeat()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)


if __name__ == "__main__":
    main()
