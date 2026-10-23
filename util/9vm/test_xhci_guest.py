#!/usr/bin/env python3
"""Boot the actual Prism driver and #X transport on QEMU, with no hostfs bridge."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import build_kernel as kernel
from prism import compile_o9

ROOT = Path(__file__).resolve().parents[2]

def main():
    subprocess.run([sys.executable, str(ROOT/'util/9vm/build_kernel.py')], check=True,
                   stdout=subprocess.DEVNULL)
    with tempfile.TemporaryDirectory(prefix='xhci-guest-') as name:
        directory = Path(name)
        probe = directory/'probe'
        compile_o9(str(ROOT/'sys/src/cmd/o9xhci/test_guest.o9'), str(probe),
                   extra_sources=[str(ROOT/'sys/src/cmd/o9xhci/xhci.c'),
                                  str(ROOT/'sys/src/cmd/o9xhci/binding.c')])
        # Reuse the existing embedded bootstrap mechanism in an isolated test
        # image. Production GEFS payloads and build/harvey.* are not replaced.
        assembly = directory/'probe.S'
        assembly.write_text('.section .rodata\n.balign 4096\n'
            '.global aspera_gefs_init_start, aspera_gefs_init_end\n'
            'aspera_gefs_init_start:\n.incbin "'+str(probe)+'"\n'
            'aspera_gefs_init_end:\n'
            '.global aspera_gefs_server_start, aspera_gefs_server_end\n'
            'aspera_gefs_server_start:\naspera_gefs_server_end:\n'
            '.global aspera_gefs_o9irc_start, aspera_gefs_o9irc_end\n'
            'aspera_gefs_o9irc_start:\naspera_gefs_o9irc_end:\n')
        obj = directory/'probe.o'
        subprocess.run([kernel.CC, '-c', str(assembly), '-o', str(obj)], check=True)
        objects = [str(ROOT/'build/kernel'/f'{Path(p).parent.name}_{Path(p).stem}.o')
                   for p in kernel.SOURCES]
        elf, image = directory/'kernel.elf', directory/'kernel.32bit'
        subprocess.run([kernel.LD, *kernel.LDFLAGS, str(obj), *objects,
                        str(ROOT/'build/libc.a'), '-o', str(elf)], check=True)
        subprocess.run([kernel.OBJCOPY, '-I', 'elf64-x86-64', '-O', 'elf32-i386',
                        str(elf), str(image)], check=True)
        disk = directory/'disk'
        with disk.open('wb') as f: f.truncate(1024*1024)
        command = [os.environ.get('QEMU', 'qemu-system-x86_64'), '-machine', 'q35',
                   '-accel', 'tcg', '-m', '512', '-display', 'none', '-serial', 'stdio',
                   '-monitor', 'none', '-no-reboot', '-kernel', str(image),
                   '-append', 'console=0 gefsroot=1',
                   '-device', 'qemu-xhci,id=xhci',
                   '-device', 'usb-kbd,bus=xhci.0,port=1',
                   '-drive', f'if=none,id=disk,file={disk},format=raw',
                   '-device', 'usb-storage,bus=xhci.0,port=2,drive=disk']
        p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            output, _ = p.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            p.terminate(); output, _ = p.communicate(timeout=5)
        text = output.decode(errors='replace')
        (ROOT/'build/quality/xhci').mkdir(parents=True, exist_ok=True)
        (ROOT/'build/quality/xhci/guest.log').write_text(text)
        if 'XHCI-GUEST PASS' not in text:
            print(text); raise SystemExit('XHCI guest test failed')
        print('\n'.join(line for line in text.splitlines() if 'xhci-guest:' in line or 'XHCI-GUEST' in line))

if __name__ == '__main__': main()
