#!/usr/bin/env python3
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def wait_port(port):
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def wait_socket(path):
    deadline = time.time() + 5
    while time.time() < deadline:
        if Path(path).exists():
            return
        time.sleep(0.05)
    raise RuntimeError("daemon socket did not appear")


def run_cli(script, *args):
    out = subprocess.check_output([sys.executable, script, *args], text=True)
    return json.loads(out)


def main():
    server_path = sys.argv[1]
    daemon_script = sys.argv[2]
    port = 7014
    sock_path = os.path.join(tempfile.gettempdir(), "luagent-clientd-test.sock")
    pid_path = os.path.join(tempfile.gettempdir(), "luagent-clientd-test.pid")
    for p in (sock_path, pid_path):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    server = subprocess.Popen([server_path, str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_port(port)
        daemon = subprocess.Popen([
            sys.executable, daemon_script, "start",
            "--agent-host", "127.0.0.1",
            "--agent-port", str(port),
            "--socket", sock_path,
            "--pid-file", pid_path,
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            wait_socket(sock_path)

            status = run_cli(daemon_script, "status", "--socket", sock_path)
            assert status["payload"]["status"] == "ok"

            op = run_cli(daemon_script, "op", "list", "path=.", "--socket", sock_path)
            assert op["payload"]["status"] == "ok"

            src = Path(tempfile.gettempdir()) / "luagent-clientd-src.txt"
            dst = Path(tempfile.gettempdir()) / "luagent-clientd-dst.txt"
            src.write_text("daemon-put-get\n")
            if dst.exists():
                dst.unlink()

            put = run_cli(daemon_script, "put", str(src), str(dst), "--socket", sock_path)
            assert put["payload"]["bytes"] == str(src.stat().st_size)

            out = Path(tempfile.gettempdir()) / "luagent-clientd-out.txt"
            if out.exists():
                out.unlink()
            get = run_cli(daemon_script, "get", str(dst), str(out), "--socket", sock_path)
            assert get["size"] == src.stat().st_size
            assert out.read_text() == src.read_text()
        finally:
            daemon.terminate()
            daemon.wait(timeout=2)
    finally:
        server.terminate()
        server.wait(timeout=2)


if __name__ == "__main__":
    main()
