#!/usr/bin/env python3
"""Exercise the production user-space xHCI engine against QEMU hardware.
No guest firmware/OS or USB mock interprets the TRBs. qtest supplies bus I/O.
"""
import ctypes as C
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
U32, U64 = C.c_uint, C.c_ulonglong
READ = C.CFUNCTYPE(C.c_int, C.c_void_p, U32, C.POINTER(U32))
WRITE = C.CFUNCTYPE(C.c_int, C.c_void_p, U32, U32)
MEM = C.CFUNCTYPE(C.c_int, C.c_void_p, U64, C.c_void_p, U32)
DELAY = C.CFUNCTYPE(None, C.c_void_p, U32)

class Io(C.Structure):
    _fields_ = [('arg', C.c_void_p), ('read32', READ), ('write32', WRITE),
                ('readmem', MEM), ('writemem', MEM), ('delay', DELAY),
                ('dma', U64), ('size', U32), ('mmiosize', U32)]

class Harness:
    def __init__(self, directory):
        self.base = 0xe0000000
        self.errors = []
        self.log = open(directory+'/qemu.log', 'w')
        # A stopped TCG CPU leaves guest RAM entirely under the harness.
        self.p = subprocess.Popen([
            os.environ.get('QEMU', 'qemu-system-x86_64'), '-machine', 'q35',
            '-accel', 'tcg', '-m', '64', '-display', 'none', '-nodefaults', '-S',
            '-qtest', f'unix:{directory}/qtest,server=on,wait=off',
            '-qtest-log', '/dev/null',
            '-qmp', f'unix:{directory}/qmp,server=on,wait=off',
            '-device', 'qemu-xhci,id=xhci,bus=pcie.0,addr=04.0',
            '-device', 'usb-kbd,id=keyboard,bus=xhci.0,port=1',
            '-drive', f'if=none,id=disk,file={directory}/disk,format=raw',
            '-device', 'usb-storage,id=storage,bus=xhci.0,port=2,drive=disk'
        ], stdout=self.log, stderr=self.log)
        self.s = self.connect(directory+'/qtest')
        self.f = self.s.makefile('rwb', buffering=0)
        self.qmp = self.connect(directory+'/qmp')
        self.qf = self.qmp.makefile('rwb', buffering=0)
        self.qf.readline()
        self.qcommand('qmp_capabilities')
        self.pciwrite(0x10, self.base)
        self.pciwrite(0x14, 0)
        self.pciwrite(4, 6)
        def rd(_, off, out):
            try:
                out[0] = int(self.call(f'readl {self.base+off:#x}')[0], 0)
                return 0
            except Exception as e:
                self.errors.append(str(e)); return -1
        def wr(_, off, value):
            try:
                self.call(f'writel {self.base+off:#x} {value:#x}')
                return 0
            except Exception as e:
                self.errors.append(str(e)); return -1
        def readmem(_, address, buf, length):
            try:
                data = bytes.fromhex(self.call(f'read {address:#x} {length:#x}')[0][2:])
                assert len(data) == length
                C.memmove(buf, data, length)
                return 0
            except Exception as e:
                self.errors.append(str(e)); return -1
        def writemem(_, address, buf, length):
            try:
                data = C.string_at(buf, length).hex()
                self.call(f'write {address:#x} {length:#x} 0x{data}')
                return 0
            except Exception as e:
                self.errors.append(str(e)); return -1
        def delay(_, milliseconds):
            # qtest clock_step is available only with CONFIG_QTEST's accelerator.
            # With a stopped TCG VM, run a tiny HLT ROM to advance USB timers.
            self.qcommand('cont')
            time.sleep(milliseconds/1000)
            self.qcommand('stop')
        self.callbacks = READ(rd), WRITE(wr), MEM(readmem), MEM(writemem), DELAY(delay)
        self.io = Io(None, *self.callbacks, 0x1000000, 4*1024*1024, 0x4000)
        # Keep firmware from touching the bus while virtual time advances.
        self.call('write 0xfffffff0 0x3 0xf4ebfd')

    def connect(self, name):
        deadline = time.monotonic()+5
        while time.monotonic()<deadline:
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(10)
            try:
                s.connect(name); return s
            except OSError:
                s.close(); time.sleep(.02)
        raise RuntimeError(Path(self.log.name).read_text())

    def call(self, text):
        self.f.write((text+'\n').encode())
        while True:
            line = self.f.readline().decode().strip()
            if line.startswith('IRQ'): continue
            assert line.startswith('OK'), (text, line)
            return line.split()[1:]

    def qcommand(self, name, args=None):
        self.qf.write((json.dumps({'execute': name, 'arguments': args or {}})+'\n').encode())
        while True:
            obj = json.loads(self.qf.readline())
            if 'event' in obj: continue
            assert 'return' in obj, obj
            return obj['return']

    def pciwrite(self, reg, value):
        self.call(f'outl 0xcf8 {0x80002000+reg:#x}')
        self.call(f'outl 0xcfc {value:#x}')

    def close(self):
        self.p.terminate()
        self.p.wait(timeout=5)
        self.f.close(); self.s.close(); self.qf.close(); self.qmp.close(); self.log.close()


def main():
    with tempfile.TemporaryDirectory(prefix='xhci-') as directory:
        libpath = directory+'/xhci.so'
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', '-g', '-O1',
                        str(ROOT/'sys/src/cmd/o9xhci/xhci.c'), '-o', libpath], check=True)
        lib = C.CDLL(libpath)
        with open(directory+'/disk', 'wb') as disk:
            disk.truncate(1024*1024)
        h = Harness(directory)
        x = C.create_string_buffer(65536)
        try:
            def check(value, label):
                assert value >= 0, (label, value, h.errors)
                return value
            check(lib.xhci_init(x, C.byref(h.io)), 'init')
            # Cross command ring and event ring cycle boundaries repeatedly.
            for _ in range(550): check(lib.xhci_noop(x), 'command wrap')
            devices = []
            for port in range(1, 9):
                status = U32()
                r = lib.xhci_port(x, port, C.byref(status))
                if r >= 0 and status.value&1:
                    slot = check(lib.xhci_attach(x, port), 'address')
                    descriptor = C.create_string_buffer(256)
                    def control(setup, n=256):
                        return check(lib.xhci_control(x, slot, setup, descriptor, n), 'control')
                    assert control(bytes([0x80,6,0,1,0,0,18,0])) == 18
                    dev = descriptor.raw[:18]
                    assert dev[1] == 1
                    assert control(bytes([0x80,6,0,2,0,0,9,0])) == 9
                    total = int.from_bytes(descriptor.raw[2:4], 'little')
                    assert total < 256
                    assert control(bytes([0x80,6,0,2,0,0,total&255,total>>8])) == total
                    config = descriptor.raw[:total]
                    check(lib.xhci_configure(x, slot, config, total), 'configure')
                    # STALL on an invalid descriptor, then recover EP0.
                    assert lib.xhci_control(x, slot, bytes([0x80,6,0,0x7f,0,0,8,0]), descriptor, 8) == -4
                    assert control(bytes([0x80,6,0,1,0,0,18,0])) == 18
                    # Short IN data followed by status completion, many EP0 wraps.
                    for _ in range(90):
                        assert control(bytes([0x80,6,0,1,0,0,64,0])) == 18
                    devices.append((slot, dev, config))
                    print(f'port {port}: slot {slot}, USB {dev[3]:x}.{dev[2]:02x}, descriptor/configuration/control PASS')
            assert len(devices) == 2, devices
            for slot, dev, config in devices:
                if dev[4] == 0 and b'\x08\x06\x50' in config:
                    # Mass-storage bulk-only SCSI INQUIRY: CBW -> data -> CSW.
                    endpoints = []
                    offset = 9
                    while offset < len(config):
                        if config[offset+1] == 5: endpoints.append(config[offset+2])
                        offset += config[offset]
                    epin = next(ep for ep in endpoints if ep&128)
                    epout = next(ep for ep in endpoints if not ep&128)
                    cbw = b'USBC'+(123).to_bytes(4,'little')+(36).to_bytes(4,'little')+bytes([128,0,6,0x12,0,0,0,36,0])+bytes(10)
                    assert len(cbw) == 31
                    for _ in range(40):
                        assert check(lib.xhci_transfer(x,slot,epout,cbw,31),'bulk out') == 31
                        data = C.create_string_buffer(65536)
                        assert check(lib.xhci_transfer(x,slot,epin,data,36),'bulk in') == 36
                        assert b'QEMU' in data.raw[:36]
                        assert check(lib.xhci_transfer(x,slot,epin,data,13),'bulk status') == 13
                        assert data.raw[:4] == b'USBS' and data.raw[12] == 0
                    print('SuperSpeed storage bulk IN/OUT and endpoint ring wrap PASS')
                else:
                    # Enable idle reports so the interrupt endpoint has data.
                    setup = bytes([0x21, 0x0a, 0, 1, 0, 0, 0, 0])
                    check(lib.xhci_control(x,slot,setup,None,0), 'keyboard idle')
                    report = C.create_string_buffer(8)
                    assert check(lib.xhci_transfer(x,slot,0x81,report,8), 'interrupt IN') == 8
                    print('keyboard interrupt IN PASS')
            for slot, _, _ in devices: check(lib.xhci_detach(x,slot), 'disable slot')
            # Removal/reconnection and Disable Slot before reusing contexts.
            h.qcommand('device_del', {'id': 'keyboard'})
            time.sleep(.05)
            h.qcommand('device_add', {'driver': 'usb-kbd', 'id': 'replacement', 'bus': 'xhci.0', 'port': '1'})
            slot = check(lib.xhci_attach(x, 5), 'reattach')
            check(lib.xhci_detach(x, slot), 'detach replacement')
            check(lib.xhci_stop(x), 'halt')
            assert not h.errors, h.errors
            print('XHCI-QEMU PASS')
        finally:
            h.close()

if __name__ == '__main__':
    main()
