#!/usr/bin/env python3
"""bench9p - host load benchmark for the esp9 9P servers.

Drives a real 9P2000 client against a server (C srv9p harness or
Go mock) and reports per-workload latency and throughput. Both
servers are single-connection; this measures sequential request
cost, which is what the ESP firmware will see.

Usage: bench9p.py HOST PORT [ITERS]
"""
import socket
import statistics
import struct
import sys
import time


def enc_str(s):
    b = s.encode()
    return struct.pack('<H', len(b)) + b


class Client:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=30)
        self.tag = 0

    def _readn(self, n):
        buf = b''
        while len(buf) < n:
            d = self.s.recv(n - len(buf))
            if not d:
                raise IOError('eof')
            buf += d
        return buf

    def rpc(self, typ, payload):
        self.tag = (self.tag + 1) & 0xFFFF
        self.s.sendall(struct.pack('<IBH', 7 + len(payload), typ,
                                   self.tag) + payload)
        size, rtyp, _ = struct.unpack('<IBH', self._readn(7))
        body = self._readn(size - 7)
        if rtyp == 107:
            n = struct.unpack('<H', body[:2])[0]
            raise RuntimeError(body[2:2 + n].decode())
        return body

    def version(self):
        self.rpc(100, struct.pack('<I', 2048) + enc_str('9P2000'))

    def attach(self):
        self.rpc(104, struct.pack('<II', 0, 0xFFFFFFFF) +
                 enc_str('') + enc_str(''))

    def walk(self, fid, newfid, path):
        names = [p for p in path.split('/') if p]
        p = struct.pack('<IIH', fid, newfid, len(names))
        for nm in names:
            p += enc_str(nm)
        self.rpc(110, p)

    def open(self, fid):
        self.rpc(112, struct.pack('<IB', fid, 0))

    def clunk(self, fid):
        self.rpc(120, struct.pack('<I', fid))

    def read(self, fid, off=0, count=1024):
        b = self.rpc(116, struct.pack('<IQI', fid, off, count))
        return struct.unpack('<I', b[:4])[0]

    def readall(self, fid):
        total, off = 0, 0
        while True:
            n = self.read(fid, off)
            if n == 0:
                return total
            total += n
            off += n

    def write(self, fid, data):
        b = data.encode()
        self.rpc(118, struct.pack('<IQI', fid, 0, len(b)) + b)


def bench(name, fn, iters):
    samples = []
    for _ in range(iters):
        t0 = time.perf_counter()
        fn()
        samples.append((time.perf_counter() - t0) * 1000.0)
    samples.sort()
    med = statistics.median(samples)
    p95 = samples[min(len(samples) - 1, int(len(samples) * 0.95))]
    print('%-28s n=%-5d med=%8.3f ms  p95=%8.3f ms  %9.1f ops/s'
          % (name, iters, med, p95, 1000.0 / med if med else 0))
    return med


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 1564
    iters = int(sys.argv[3]) if len(sys.argv) > 3 else 300

    c = Client(host, port)
    t0 = time.perf_counter()
    c.version()
    c.attach()
    print('setup version+attach: %.3f ms'
          % ((time.perf_counter() - t0) * 1000.0))

    c.walk(0, 1, '/status')
    c.open(1)
    bench('Tread /status (open fid)', lambda: c.read(1), iters)
    bench('readall /status', lambda: c.readall(1), iters // 3)

    fid = [10]

    def walk_open_read():
        f = fid[0]
        fid[0] += 1
        c.walk(0, f, '/version')
        c.open(f)
        c.readall(f)
        c.clunk(f)

    bench('walk+open+read+clunk /version', walk_open_read, iters // 3)

    c.walk(0, 2, '/wifi/ctl')
    c.open(2)
    bench("Twrite 'scan' /wifi/ctl", lambda: c.write(2, 'scan'),
          iters // 10)
    print('BENCH9P: DONE')


if __name__ == '__main__':
    main()
