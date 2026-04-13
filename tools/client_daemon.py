#!/usr/bin/env python3
import argparse
import atexit
import json
import os
import signal
import socket
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

MAGIC = b"RXSH"
FRAME_HELLO = 1
FRAME_ACK = 2
FRAME_ERROR = 3
FRAME_OP = 10
FRAME_OP_RESULT = 11
FRAME_PUT_BEGIN = 20
FRAME_PUT_CHUNK = 21
FRAME_PUT_END = 22
FRAME_GET_BEGIN = 30
FRAME_GET_CHUNK = 31
FRAME_GET_END = 32
FRAME_PROC_SPAWN = 40
FRAME_STDOUT = 42
FRAME_STDERR = 43
FRAME_EXIT = 44
FRAME_KILL = 45
FRAME_PING = 50
FRAME_PONG = 51

DEFAULT_SOCK = os.path.join(tempfile.gettempdir(), "luagent-clientd.sock")
DEFAULT_PID = os.path.join(tempfile.gettempdir(), "luagent-clientd.pid")


def encode_frame(msg_type, req_id, payload=b""):
    return MAGIC + bytes([msg_type, 0]) + struct.pack("<II", req_id, len(payload)) + payload


def recv_exact(sock, n):
    out = b""
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise RuntimeError("connection closed")
        out += chunk
    return out


def decode_frame(sock):
    hdr = recv_exact(sock, 14)
    if hdr[:4] != MAGIC:
        raise RuntimeError("bad magic")
    msg_type = hdr[4]
    req_id, length = struct.unpack("<II", hdr[6:14])
    payload = recv_exact(sock, length)
    return msg_type, req_id, payload


def parse_kv(payload):
    out = {}
    for line in payload.decode("utf-8", errors="replace").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            out[key] = value
    return out


def encode_kv(items):
    lines = []
    for key, value in items.items():
        lines.append(f"{key}={value}")
    return "\n".join(lines).encode()


class AgentConnection:
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock_lock = threading.Lock()
        self.req_lock = threading.Lock()
        self.next_req_id = 1000
        self.pending = {}
        self.streams = {}
        self.stream_cond = threading.Condition()
        self.running = True
        self.reader = threading.Thread(target=self._reader, daemon=True)
        self._hello()
        self.reader.start()

    def _hello(self):
        with self.sock_lock:
            self.sock.sendall(encode_frame(FRAME_HELLO, 1, b"client=linux-daemon\nversion=1"))
            msg_type, req_id, payload = decode_frame(self.sock)
        if msg_type != FRAME_ACK or req_id != 1:
            raise RuntimeError(f"HELLO failed: type={msg_type} req={req_id} payload={payload!r}")

    def close(self):
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass

    def _reader(self):
        try:
            while self.running:
                msg_type, req_id, payload = decode_frame(self.sock)
                if msg_type == FRAME_PING:
                    with self.sock_lock:
                        self.sock.sendall(encode_frame(FRAME_PONG, req_id, b"status=ok"))
                    continue
                if msg_type in (FRAME_STDOUT, FRAME_STDERR, FRAME_EXIT, FRAME_GET_CHUNK, FRAME_GET_END):
                    with self.stream_cond:
                        self.streams.setdefault(req_id, []).append((msg_type, payload))
                        self.stream_cond.notify_all()
                    continue
                self.pending[req_id] = (msg_type, payload)
        except Exception:
            self.running = False

    def _next_req(self):
        with self.req_lock:
            req = self.next_req_id
            self.next_req_id += 1
            return req

    def request(self, msg_type, payload=b"", timeout=10):
        req_id = self._next_req()
        with self.sock_lock:
            self.sock.sendall(encode_frame(msg_type, req_id, payload))
        deadline = time.time() + timeout
        while time.time() < deadline:
            if req_id in self.pending:
                resp = self.pending.pop(req_id)
                return req_id, resp[0], resp[1]
            time.sleep(0.01)
        raise TimeoutError("request timed out")

    def request_op(self, kv_items, timeout=10):
        return self.request(FRAME_OP, encode_kv(kv_items), timeout=timeout)

    def put_file(self, local_path, remote_path):
        data = Path(local_path).read_bytes()
        req_id, msg_type, payload = self.request(FRAME_PUT_BEGIN, encode_kv({
            "path": remote_path,
            "size": len(data),
            "overwrite": 1
        }))
        if msg_type == FRAME_ERROR:
            raise RuntimeError(parse_kv(payload))
        with self.sock_lock:
            for off in range(0, len(data), 4096):
                self.sock.sendall(encode_frame(FRAME_PUT_CHUNK, req_id, data[off:off + 4096]))
            self.sock.sendall(encode_frame(FRAME_PUT_END, req_id, b""))
        deadline = time.time() + 10
        while time.time() < deadline:
            if req_id in self.pending:
                msg_type, payload = self.pending.pop(req_id)
                return {"type": msg_type, "payload": parse_kv(payload)}
            time.sleep(0.01)
        raise TimeoutError("PUT_END timed out")

    def get_file(self, remote_path, local_path):
        req_id, msg_type, payload = self.request(FRAME_GET_BEGIN, encode_kv({
            "path": remote_path,
            "chunk_size": 4096
        }))
        if msg_type == FRAME_ERROR:
            raise RuntimeError(parse_kv(payload))
        out = bytearray()
        deadline = time.time() + 10
        while time.time() < deadline:
            with self.stream_cond:
                items = self.streams.get(req_id, [])
                if items:
                    msg_type, payload = items.pop(0)
                else:
                    self.stream_cond.wait(timeout=0.1)
                    continue
            if msg_type == FRAME_GET_CHUNK:
                out.extend(payload)
            elif msg_type == FRAME_GET_END:
                Path(local_path).write_bytes(out)
                return {"size": len(out)}
        raise TimeoutError("GET timed out")

    def spawn(self, path, argv, timeout_ms=60000, idle_timeout_ms=10000):
        payload = {"path": path, "timeout_ms": timeout_ms, "idle_timeout_ms": idle_timeout_ms}
        for idx, arg in enumerate(argv):
            payload[f"argv{idx}"] = arg
        _, msg_type, resp = self.request(FRAME_PROC_SPAWN, encode_kv(payload))
        info = parse_kv(resp)
        if msg_type == FRAME_ERROR:
            raise RuntimeError(info)
        return info

    def kill(self, proc_id):
        _, msg_type, resp = self.request(FRAME_KILL, encode_kv({"proc_id": proc_id}))
        info = parse_kv(resp)
        if msg_type == FRAME_ERROR:
            raise RuntimeError(info)
        return info

    def collect_stream(self, proc_id, timeout=10):
        deadline = time.time() + timeout
        stdout = bytearray()
        stderr = bytearray()
        exit_info = None
        while time.time() < deadline:
            with self.stream_cond:
                items = self.streams.get(int(proc_id), [])
                if items:
                    msg_type, payload = items.pop(0)
                else:
                    self.stream_cond.wait(timeout=0.1)
                    continue
            if msg_type == FRAME_STDOUT:
                stdout.extend(payload)
            elif msg_type == FRAME_STDERR:
                stderr.extend(payload)
            elif msg_type == FRAME_EXIT:
                exit_info = parse_kv(payload)
                break
        return {
            "stdout": stdout.decode("utf-8", errors="replace"),
            "stderr": stderr.decode("utf-8", errors="replace"),
            "exit": exit_info,
        }


class ControlServer:
    def __init__(self, agent_host, agent_port, sock_path):
        self.agent = AgentConnection(agent_host, agent_port)
        self.sock_path = sock_path
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            os.unlink(sock_path)
        except FileNotFoundError:
            pass
        self.server.bind(sock_path)
        self.server.listen(16)
        self.running = True

    def close(self):
        self.running = False
        try:
            self.server.close()
        except OSError:
            pass
        try:
            os.unlink(self.sock_path)
        except FileNotFoundError:
            pass
        self.agent.close()

    def serve(self):
        while self.running:
            try:
                conn, _ = self.server.accept()
            except OSError:
                break
            threading.Thread(target=self.handle_client, args=(conn,), daemon=True).start()

    def handle_client(self, conn):
        with conn:
            data = b""
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                data += chunk
                if b"\n" in data:
                    line, _, _ = data.partition(b"\n")
                    break
            request = json.loads(line.decode())
            try:
                response = self.dispatch(request)
                payload = {"ok": True, "result": response}
            except Exception as exc:
                payload = {"ok": False, "error": str(exc)}
            conn.sendall((json.dumps(payload) + "\n").encode())

    def dispatch(self, request):
        cmd = request["cmd"]
        if cmd == "status":
            _, msg_type, payload = self.agent.request_op({"name": "debug.stats"})
            return {"type": msg_type, "payload": parse_kv(payload)}
        if cmd == "op":
            _, msg_type, payload = self.agent.request_op(request["kv"])
            return {"type": msg_type, "payload": parse_kv(payload)}
        if cmd == "put":
            return self.agent.put_file(request["local"], request["remote"])
        if cmd == "get":
            return self.agent.get_file(request["remote"], request["local"])
        if cmd == "spawn":
            return self.agent.spawn(
                request["path"],
                request["argv"],
                timeout_ms=request.get("timeout_ms", 60000),
                idle_timeout_ms=request.get("idle_timeout_ms", 10000),
            )
        if cmd == "kill":
            return self.agent.kill(request["proc_id"])
        if cmd == "wait-proc":
            return self.agent.collect_stream(request["proc_id"], timeout=request.get("timeout", 10))
        raise RuntimeError(f"unknown command: {cmd}")


def daemonize():
    if os.fork() > 0:
        os._exit(0)
    os.setsid()
    if os.fork() > 0:
        os._exit(0)
    sys.stdout.flush()
    sys.stderr.flush()
    with open(os.devnull, "rb", 0) as rd, open(os.devnull, "ab", 0) as wr:
        os.dup2(rd.fileno(), 0)
        os.dup2(wr.fileno(), 1)
        os.dup2(wr.fileno(), 2)


def send_control(sock_path, payload):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(sock_path)
        s.sendall((json.dumps(payload) + "\n").encode())
        data = b""
        while not data.endswith(b"\n"):
            data += s.recv(4096)
    result = json.loads(data.decode())
    if not result["ok"]:
        raise RuntimeError(result["error"])
    return result["result"]


def parse_kv_args(items):
    out = {}
    for item in items:
        if "=" not in item:
            raise ValueError(f"expected key=value, got {item}")
        key, value = item.split("=", 1)
        out[key] = value
    return out


def cmd_start(args):
    if args.daemonize:
        daemonize()
    server = ControlServer(args.agent_host, args.agent_port, args.socket)
    Path(args.pid_file).write_text(str(os.getpid()))
    atexit.register(lambda: Path(args.pid_file).unlink(missing_ok=True))
    atexit.register(server.close)
    signal.signal(signal.SIGTERM, lambda *_: (server.close(), sys.exit(0)))
    server.serve()


def cmd_stop(args):
    pid = int(Path(args.pid_file).read_text().strip())
    os.kill(pid, signal.SIGTERM)


def cmd_status(args):
    print(json.dumps(send_control(args.socket, {"cmd": "status"}), indent=2))


def cmd_op(args):
    kv = parse_kv_args(args.kv)
    kv["name"] = args.name
    print(json.dumps(send_control(args.socket, {"cmd": "op", "kv": kv}), indent=2))


def cmd_put(args):
    print(json.dumps(send_control(args.socket, {
        "cmd": "put", "local": args.local, "remote": args.remote
    }), indent=2))


def cmd_get(args):
    print(json.dumps(send_control(args.socket, {
        "cmd": "get", "remote": args.remote, "local": args.local
    }), indent=2))


def cmd_spawn(args):
    print(json.dumps(send_control(args.socket, {
        "cmd": "spawn",
        "path": args.path,
        "argv": [args.path] + args.argv,
        "timeout_ms": args.timeout_ms,
        "idle_timeout_ms": args.idle_timeout_ms,
    }), indent=2))


def cmd_wait_proc(args):
    print(json.dumps(send_control(args.socket, {
        "cmd": "wait-proc", "proc_id": args.proc_id, "timeout": args.timeout
    }), indent=2))


def cmd_kill(args):
    print(json.dumps(send_control(args.socket, {
        "cmd": "kill", "proc_id": args.proc_id
    }), indent=2))


def build_parser():
    p = argparse.ArgumentParser(description="Linux-side luagent client daemon")
    sub = p.add_subparsers(dest="sub", required=True)

    start = sub.add_parser("start")
    start.add_argument("--agent-host", default="127.0.0.1")
    start.add_argument("--agent-port", type=int, default=7000)
    start.add_argument("--socket", default=DEFAULT_SOCK)
    start.add_argument("--pid-file", default=DEFAULT_PID)
    start.add_argument("--daemonize", action="store_true")
    start.set_defaults(func=cmd_start)

    stop = sub.add_parser("stop")
    stop.add_argument("--pid-file", default=DEFAULT_PID)
    stop.set_defaults(func=cmd_stop)

    status = sub.add_parser("status")
    status.add_argument("--socket", default=DEFAULT_SOCK)
    status.set_defaults(func=cmd_status)

    op = sub.add_parser("op")
    op.add_argument("name")
    op.add_argument("kv", nargs="*")
    op.add_argument("--socket", default=DEFAULT_SOCK)
    op.set_defaults(func=cmd_op)

    put = sub.add_parser("put")
    put.add_argument("local")
    put.add_argument("remote")
    put.add_argument("--socket", default=DEFAULT_SOCK)
    put.set_defaults(func=cmd_put)

    get = sub.add_parser("get")
    get.add_argument("remote")
    get.add_argument("local")
    get.add_argument("--socket", default=DEFAULT_SOCK)
    get.set_defaults(func=cmd_get)

    spawn = sub.add_parser("spawn")
    spawn.add_argument("path")
    spawn.add_argument("argv", nargs="*")
    spawn.add_argument("--socket", default=DEFAULT_SOCK)
    spawn.add_argument("--timeout-ms", type=int, default=60000)
    spawn.add_argument("--idle-timeout-ms", type=int, default=10000)
    spawn.set_defaults(func=cmd_spawn)

    waitp = sub.add_parser("wait-proc")
    waitp.add_argument("proc_id", type=int)
    waitp.add_argument("--timeout", type=int, default=10)
    waitp.add_argument("--socket", default=DEFAULT_SOCK)
    waitp.set_defaults(func=cmd_wait_proc)

    kill = sub.add_parser("kill")
    kill.add_argument("proc_id", type=int)
    kill.add_argument("--socket", default=DEFAULT_SOCK)
    kill.set_defaults(func=cmd_kill)

    return p


def main():
    parser = build_parser()
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
