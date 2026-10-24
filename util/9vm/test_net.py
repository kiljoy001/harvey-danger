#!/usr/bin/env python3
"""Guest networking test for the 9P-only microVM.

There is no in-kernel network stack.  util/9vm/netfs exports /net over
VirtIO 9P; this test starts host TCP and UDP echo servers, boots a guest that
mounts #9/1, and drives dial(2) through /net/cs, /net/tcp, and /net/udp.
"""

import os
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
PRISM = os.path.join(ROOT, "util/9vm/prism.py")
TEST_O9 = os.path.join(ROOT, "boot/test_netfs.o9")
TCP_PORT = 19996
UDP_PORT = 19997
DUPLEX_PORT = 19998
SERVER_PORT = 19999

REQUIRED = [
    "o9-netfs: mounted #9/1 -> /net",
    "o9-netfs: tcp echo OK",
    "o9-netfs: udp echo OK",
    "o9-netfs: PASS tcp and udp",
    "o9-netfs: full-duplex read/write OK",
    "o9-netfs: announce/listen/accept OK",
    "o9-netfs: done ok=1 full_duplex=1 server=1",
]


def tcp_echo(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(4)

    def handle(conn):
        with conn:
            while True:
                data = conn.recv(4096)
                if not data:
                    return
                conn.sendall(data)

    def run():
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            threading.Thread(target=handle, args=(conn,), daemon=True).start()

    threading.Thread(target=run, daemon=True).start()
    return srv


def pong_server(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(4)

    def handle(conn):
        with conn:
            data = conn.recv(4)
            if data == b"ping":
                conn.sendall(b"pong")

    def run():
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            threading.Thread(target=handle, args=(conn,), daemon=True).start()

    threading.Thread(target=run, daemon=True).start()
    return srv


def guest_server_client(port, result):
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            conn = socket.create_connection(("127.0.0.1", port), timeout=1)
        except OSError:
            time.sleep(0.05)
            continue
        try:
            conn.sendall(b"guest-server")
            data = b""
            while len(data) < 8:
                chunk = conn.recv(8 - len(data))
                if not chunk:
                    break
                data += chunk
            result.append(data)
        except OSError:
            result.append(None)
        finally:
            conn.close()
        return
    result.append(None)


def udp_echo(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))

    def run():
        while True:
            try:
                data, peer = srv.recvfrom(4096)
            except OSError:
                return
            srv.sendto(data, peer)

    threading.Thread(target=run, daemon=True).start()
    return srv


def main():
    tcp = tcp_echo(TCP_PORT)
    udp = udp_echo(UDP_PORT)
    duplex = pong_server(DUPLEX_PORT)
    server_result = []
    server_thread = threading.Thread(
        target=guest_server_client, args=(SERVER_PORT, server_result), daemon=True)
    server_thread.start()
    try:
        start = time.perf_counter()
        res = subprocess.run(
            [sys.executable, PRISM, "run", TEST_O9, "-v", "-t", "30"],
            cwd=ROOT, capture_output=True, text=True, timeout=45)
        elapsed = time.perf_counter() - start
        combined = res.stdout + res.stderr

        failed = False
        for want in REQUIRED:
            ok = want in combined
            print(f"  [{'PASS' if ok else 'FAIL'}] {want}")
            failed |= not ok
        server_thread.join(timeout=2)
        server_ok = bool(server_result and server_result[0] == b"guest-ok")
        print(f"  [{'PASS' if server_ok else 'FAIL'}] guest accepted host connection")
        failed |= not server_ok
        if res.returncode != 0:
            print(f"  [FAIL] VM exit code {res.returncode}")
            failed = True
        if failed:
            print("\n--- guest output ---\n" + combined)
            raise SystemExit(1)
        print(f"netfs guest networking OK ({elapsed:.2f}s)")
    finally:
        tcp.close()
        udp.close()
        duplex.close()


if __name__ == "__main__":
    main()
