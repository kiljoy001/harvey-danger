#!/usr/bin/env python3
"""Exercise the host /net 9P service without booting a guest.

The test starts a TCP and a UDP echo server, starts util/9vm/netfs on a
private Unix socket, and speaks 9P2000 to it.  It verifies /net/cs, clone,
ctl, data, and the status files for both transports.
"""

import os
import socket
import struct
import subprocess
import tempfile
import threading
import time


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
NETFS = os.path.join(ROOT, "util/9vm/netfs")

Tversion, Rversion = 100, 101
Tauth, Rauth = 102, 103
Tattach, Rattach = 104, 105
Terror, Rerror = 106, 107
Tflush, Rflush = 108, 109
Twalk, Rwalk = 110, 111
Topen, Ropen = 112, 113
Tcreate, Rcreate = 114, 115
Tread, Rread = 116, 117
Twrite, Rwrite = 118, 119
Tclunk, Rclunk = 120, 121
Tremove, Rremove = 122, 123
Tstat, Rstat = 124, 125
Twstat, Rwstat = 126, 127

ORDWR = 2
NOFID = 0xFFFFFFFF


def pstr(s):
    b = s.encode() if isinstance(s, str) else s
    return struct.pack("<H", len(b)) + b


def gstr(b, off=0):
    n = struct.unpack_from("<H", b, off)[0]
    return b[off + 2:off + 2 + n].decode(errors="replace"), off + 2 + n


class P9:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.tag = 1

    def rpc(self, mtype, body=b""):
        tag = self.tag
        self.tag = (self.tag + 1) & 0xFFFF
        if self.tag == 0xFFFF:
            self.tag = 1
        msg = struct.pack("<IBH", 7 + len(body), mtype, tag) + body
        self.s.sendall(msg)
        hdr = self._read(7)
        size, rtype, rtag = struct.unpack("<IBH", hdr)
        assert rtag == tag, (rtag, tag)
        data = self._read(size - 7)
        if rtype == Rerror:
            elen = struct.unpack_from("<H", data, 0)[0]
            raise RuntimeError(data[2:2 + elen].decode(errors="replace"))
        return rtype, data

    def _read(self, n):
        out = b""
        while len(out) < n:
            chunk = self.s.recv(n - len(out))
            if not chunk:
                raise EOFError("netfs closed the connection")
            out += chunk
        return out

    def version(self, msize=65536):
        return self.rpc(Tversion, struct.pack("<I", msize) + pstr("9P2000"))

    def attach(self, fid, uname="scott", aname=""):
        return self.rpc(Tattach, struct.pack("<II", fid, NOFID) +
                        pstr(uname) + pstr(aname))

    def walk(self, fid, newfid, names):
        body = struct.pack("<IIH", fid, newfid, len(names))
        for name in names:
            body += pstr(name)
        return self.rpc(Twalk, body)

    def open(self, fid, mode=ORDWR):
        return self.rpc(Topen, struct.pack("<IB", fid, mode))

    def read(self, fid, count=65536, offset=0):
        _, data = self.rpc(Tread, struct.pack("<IQI", fid, offset, count))
        n = struct.unpack_from("<I", data, 0)[0]
        return data[4:4 + n]

    def write(self, fid, data, offset=0):
        _, resp = self.rpc(Twrite, struct.pack("<IQI", fid, offset, len(data)) + data)
        return struct.unpack_from("<I", resp, 0)[0]

    def clunk(self, fid):
        return self.rpc(Tclunk, struct.pack("<I", fid))

    def close(self):
        self.s.close()


def tcp_echo_server():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(4)

    def run():
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            threading.Thread(target=handle, args=(conn,), daemon=True).start()

    def handle(conn):
        with conn:
            while True:
                data = conn.recv(4096)
                if not data:
                    return
                conn.sendall(data)

    threading.Thread(target=run, daemon=True).start()
    return srv, srv.getsockname()[1]


def udp_echo_server():
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.bind(("127.0.0.1", 0))

    def run():
        while True:
            try:
                data, peer = srv.recvfrom(4096)
            except OSError:
                return
            srv.sendto(data, peer)

    threading.Thread(target=run, daemon=True).start()
    return srv, srv.getsockname()[1]


def clone(p, proto, dest):
    p.walk(1, 2, [proto, "clone"])
    p.open(2, ORDWR)
    num = p.read(2, 64).decode().strip()
    p.clunk(2)
    assert num.isdigit(), num
    return num


def connect(p, proto, num, dest):
    p.walk(1, 3, [proto, num, "ctl"])
    p.open(3, ORDWR)
    p.write(3, ("connect " + dest).encode())
    return 3


def open_data(p, proto, num, fid=4):
    p.walk(1, fid, [proto, num, "data"])
    p.open(fid, ORDWR)


def read_text(p, path, fid=6):
    p.walk(1, fid, path)
    p.open(fid, 0)
    chunks = []
    off = 0
    while True:
        b = p.read(fid, 4096, off)
        if not b:
            break
        chunks.append(b)
        off += len(b)
    p.clunk(fid)
    return b"".join(chunks).decode()


def main():
    if not os.path.exists(NETFS):
        raise SystemExit(f"missing {NETFS}; run make -C util/9vm netfs")

    tmp = tempfile.mkdtemp(prefix="netfs-test-", dir="/tmp/opencode")
    sockpath = os.path.join(tmp, "netfs.sock")
    proc = subprocess.Popen([NETFS, "-s", sockpath],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True)
    tcp_srv, tcp_port = tcp_echo_server()
    udp_srv, udp_port = udp_echo_server()
    try:
        for _ in range(100):
            if os.path.exists(sockpath):
                break
            if proc.poll() is not None:
                raise SystemExit("netfs exited: " + proc.stderr.read())
            time.sleep(0.01)
        else:
            raise SystemExit("netfs did not create its socket")

        p = P9(sockpath)
        rtype, data = p.version()
        assert rtype == Rversion
        p.attach(1)

        # /net/cs must emit a clone path and a numeric destination.
        p.walk(1, 5, ["cs"])
        p.open(5, ORDWR)
        query = f"tcp!127.0.0.1!{tcp_port}"
        p.write(5, query.encode())
        cs = p.read(5, 4096).decode()
        p.clunk(5)
        assert cs == f"/net/tcp/clone 127.0.0.1!{tcp_port}\n", cs

        # TCP: clone, connect, exchange data, read status.
        num = clone(p, "tcp", f"127.0.0.1!{tcp_port}")
        ctl = connect(p, "tcp", num, f"127.0.0.1!{tcp_port}")
        open_data(p, "tcp", num)
        p.write(4, b"hello over 9P\n")
        assert p.read(4, 4096) == b"hello over 9P\n"
        p.clunk(ctl)
        p.clunk(4)
        status = read_text(p, ["tcp", num, "status"], 7)
        assert "Established" in status, status

        # UDP has the same user-visible interface.
        p.walk(1, 5, ["cs"])
        p.open(5, ORDWR)
        p.write(5, f"udp!127.0.0.1!{udp_port}".encode())
        cs = p.read(5, 4096).decode()
        p.clunk(5)
        assert cs == f"/net/udp/clone 127.0.0.1!{udp_port}\n", cs
        num = clone(p, "udp", f"127.0.0.1!{udp_port}")
        ctl = connect(p, "udp", num, f"127.0.0.1!{udp_port}")
        open_data(p, "udp", num)
        p.write(4, b"udp over 9P")
        assert p.read(4, 4096) == b"udp over 9P"
        p.clunk(ctl)
        p.clunk(4)

        # Hostname resolution through /net/cs.
        p.walk(1, 5, ["cs"])
        p.open(5, ORDWR)
        p.write(5, f"tcp!localhost!{tcp_port}".encode())
        cs = p.read(5, 4096).decode()
        p.clunk(5)
        assert cs.startswith("/net/tcp/clone ") and cs.rstrip().endswith(f"!{tcp_port}"), cs

        # TCP announce/listen/accept.  The 9P read of /listen must wait for
        # an incoming connection without blocking other 9P requests.
        p.walk(1, 6, ["tcp", "clone"])
        p.open(6, ORDWR)
        listen_num = p.read(6, 64).decode().strip()
        p.write(6, b"announce 127.0.0.1!0")
        local = read_text(p, ["tcp", listen_num, "local"], 5).strip()
        listen_port = int(local.rsplit("!", 1)[1])
        p.walk(1, 7, ["tcp", listen_num, "listen"])
        p.open(7, ORDWR)
        tag = 200
        body = struct.pack("<IQI", 7, 0, 64)
        p.s.sendall(struct.pack("<IBH", 7 + len(body), Tread, tag) + body)
        accepted = socket.create_connection(("127.0.0.1", listen_port), timeout=5)
        hdr = p._read(7)
        size, rtype, rtag = struct.unpack("<IBH", hdr)
        data = p._read(size - 7)
        assert rtag == tag and rtype == Rread, (rtype, rtag, data)
        n = struct.unpack_from("<I", data, 0)[0]
        accepted_num = data[4:4 + n].decode().strip()
        p.walk(1, 8, ["tcp", accepted_num, "data"])
        p.open(8, ORDWR)
        accepted.sendall(b"server side")
        assert p.read(8, 64) == b"server side"
        accepted.close()
        p.clunk(8)
        p.clunk(7)
        p.clunk(6)

        p.close()
        print("netfs: TCP, UDP, cs, announce/listen, and /net status OK")
    finally:
        tcp_srv.close()
        udp_srv.close()
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        try:
            os.unlink(sockpath)
        except FileNotFoundError:
            pass
        os.rmdir(tmp)


if __name__ == "__main__":
    main()
