#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
src="$root/sys/src/cmd/o9xhci"
out="$root/build/quality/xhci"
cc=${CC:-clang-18}
mkdir -p "$out"
flags='-std=c11 -Wall -Wextra -Werror'
$cc $flags -O2 "$src/test_xhci.c" -o "$out/unit"
"$out/unit"
$cc $flags -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$src/test_xhci.c" -o "$out/sanitized"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/sanitized"
$cc $flags -O1 -g -fsanitize=fuzzer,address,undefined \
    "$src/xhci.c" "$src/fuzz_xhci.c" -o "$out/fuzz"
mkdir -p "$out/corpus"
python3 - "$out/corpus/config" <<'PY'
import sys
from pathlib import Path
Path(sys.argv[1]).write_bytes(bytes([
    9,2,31,0,1,1,0,128,50,9,4,0,0,1,8,6,80,0,
    7,5,129,2,0,4,0,6,48,0,0,0,0]))
PY
"$out/fuzz" -runs="${FUZZ_RUNS:-20000}" -max_len=4096 "$out/corpus" >"$out/fuzz.log" 2>&1
major=$($cc -dumpversion | cut -d. -f1)
profdata=llvm-profdata-$major
cov=llvm-cov-$major
$cc $flags -O0 -g -fprofile-instr-generate -fcoverage-mapping \
    "$src/test_xhci.c" -o "$out/coverage"
LLVM_PROFILE_FILE="$out/xhci.profraw" "$out/coverage"
"$profdata" merge -sparse "$out/xhci.profraw" -o "$out/xhci.profdata"
"$cov" export "$out/coverage" -instr-profile="$out/xhci.profdata" >"$out/coverage.json"
python3 "$root/util/quality/crap.py" --maximum 30 --source "$src/xhci.c" \
    --output "$out/crap.json" "$out/coverage.json"
python3 - "$src" "$out" "$cc" <<'PY'
import pathlib, subprocess, sys, tempfile
src,out,cc=pathlib.Path(sys.argv[1]),pathlib.Path(sys.argv[2]),sys.argv[3]
changes=[('cycle','r->cycle ^= 1','r->cycle ^= 0'),
         ('timeout','return bad(x, Xtimeout)','return 0'),
         ('residual','length-(v[2]&0xffffff)','length+(v[2]&0xffffff)'),
         ('length','len>n-off','len>n'),
         ('duplicate','if(c->seen&BIT(ep))','if(0)'),
         ('dma','a < x->io.dma','0')]
with tempfile.TemporaryDirectory(dir=out) as tmp:
    d=pathlib.Path(tmp)
    for name in ('xhci.h','test_xhci.c'): (d/name).write_bytes((src/name).read_bytes())
    original=(src/'xhci.c').read_text()
    for name,old,new in changes:
        assert old in original, name
        (d/'xhci.c').write_text(original.replace(old,new))
        subprocess.run([cc,'-O1','-g','-fsanitize=address,undefined',str(d/'test_xhci.c'),'-o',str(d/'mutant')],check=True)
        result=subprocess.run([str(d/'mutant')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        assert result.returncode!=0, f'surviving mutant: {name}'
        print(f'xhci mutation killed: {name}')
PY
echo XHCI-NATIVE-QUALITY-PASS
