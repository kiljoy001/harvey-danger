#!/usr/bin/env python3
"""Headless 9vm display consumer: drain committed frames and save the latest."""
import ctypes, mmap, os, sys, time

W, H = 1024, 768
SLOTS = 2
EVENTS = 256
EVENT_BYTES = 96
FREE, WRITING, READY, READING = 0, 1, 2, 3

class VmDisplay(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32), ("version", ctypes.c_uint32),
        ("width", ctypes.c_uint32), ("height", ctypes.c_uint32),
        ("latest_serial", ctypes.c_uint64), ("latest_slot", ctypes.c_uint32),
        ("state", ctypes.c_uint32 * SLOTS), ("written", ctypes.c_uint32 * SLOTS),
        ("slot_serial", ctypes.c_uint64 * SLOTS),
        ("input_head", ctypes.c_uint32), ("input_tail", ctypes.c_uint32),
        ("input", (ctypes.c_char * EVENT_BYTES) * EVENTS),
        ("pixels", (ctypes.c_uint32 * (W * H)) * SLOTS),
    ]

def writeppm(path, px):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (W, H))
        buf = bytearray(W * H * 3)
        for i in range(W * H):
            c = px[i]
            buf[i*3+0] = (c >> 16) & 0xFF
            buf[i*3+1] = (c >> 8) & 0xFF
            buf[i*3+2] = c & 0xFF
        f.write(buf)

def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "/itest"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    out = sys.argv[3] if len(sys.argv) > 3 else "/tmp/opencode/frame.ppm"
    path = "/dev/shm/" + name.lstrip("/")
    with open(path, "r+b") as f:
        mm = mmap.mmap(f.fileno(), os.fstat(f.fileno()).st_size)
        d = VmDisplay.from_buffer(mm)
        seen = 0
        end = time.time() + seconds
        frames = 0
        while time.time() < end:
            latest = d.latest_serial
            slot = d.latest_slot
            if latest > seen and slot < SLOTS and d.slot_serial[slot] == latest \
               and d.state[slot] == READY:
                d.state[slot] = READING
                px = list(d.pixels[slot])
                d.state[slot] = FREE
                seen = latest
                writeppm(out, px)
                frames += 1
                print("frame %d serial %d slot %d" % (frames, latest, slot), flush=True)
            time.sleep(0.02)
        print("drain done, %d frames" % frames)

main()
