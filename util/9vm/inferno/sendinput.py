#!/usr/bin/env python3
"""Write input event records to the 9vm display shared memory."""
import ctypes, mmap, os, sys, time

SLOTS = 2
EVENTS = 256
EVENT_BYTES = 96
W, H = 1024, 768

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

def main():
    name = sys.argv[1]
    path = "/dev/shm/" + name.lstrip("/")
    with open(path, "r+b") as f:
        mm = mmap.mmap(f.fileno(), os.fstat(f.fileno()).st_size)
        d = VmDisplay.from_buffer(mm)
        for rec in sys.argv[2:]:
            if not rec.endswith("\n"):
                rec += "\n"
            head = d.input_head
            tail = d.input_tail
            if head - tail >= EVENTS:
                print("input ring full")
                return
            slot = head % EVENTS
            b = rec.encode()
            d.input[slot][:len(b)] = b
            d.input[slot][len(b):len(b)+1] = b"\0"
            d.input_head = head + 1
            print("sent", repr(rec), "head", head + 1)
            time.sleep(0.05)

main()
