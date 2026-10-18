#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
source_dir="$root/sys/src/cmd/o9drvman"
include_dir="$root/sys/include"
quality_dir="$root/util/quality"
build_dir="$root/build/quality/o9drvman"
fuzz_runs=${FUZZ_RUNS:-20000}

mkdir -p "$build_dir"

if test -n "${CC:-}"; then
	cc=$CC
else
	cc=
	for candidate in clang-20 clang-18 clang; do
		if command -v "$candidate" >/dev/null 2>&1 &&
		   printf 'int main(void){return 0;}\n' |
		   "$candidate" -x c -fsanitize=address -o "$build_dir/.cc-probe" -
		then
			cc=$candidate
			break
		fi
	done
	if test -z "$cc"; then
		echo "quality: no AddressSanitizer capable Clang found" >&2
		exit 1
	fi
	rm -f "$build_dir/.cc-probe"
fi
common="-std=c11 -Wall -Wextra -Werror -I$source_dir"
kernel_common="-std=c11 -Wall -Wextra -Werror -iquote$include_dir"
echo "quality: compiler $cc"

echo "quality: deterministic unit tests"
$cc $common -O2 "$source_dir/lifecycle.c" "$source_dir/test_lifecycle.c" \
	-o "$build_dir/test_lifecycle"
"$build_dir/test_lifecycle" --unit

echo "quality: generated property tests"
"$build_dir/test_lifecycle" --property

echo "quality: address and undefined behavior sanitizers"
$cc $common -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	"$source_dir/lifecycle.c" "$source_dir/test_lifecycle.c" \
	-o "$build_dir/test_lifecycle_san"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/test_lifecycle_san"

echo "quality: coverage guided fuzzing ($fuzz_runs runs)"
$cc $common -O1 -g -fno-omit-frame-pointer \
	-fsanitize=fuzzer,address,undefined \
	"$source_dir/lifecycle.c" "$source_dir/fuzz_lifecycle.c" \
	-o "$build_dir/fuzz_lifecycle"
fuzz_log="$build_dir/fuzz.log"
if ! ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/fuzz_lifecycle" -runs="$fuzz_runs" -max_len=1024 \
	-artifact_prefix="$build_dir/" >"$fuzz_log" 2>&1
then
	tail -200 "$fuzz_log" >&2
	exit 1
fi

echo "quality: kernel transport unit tests"
$cc $kernel_common -O2 "$quality_dir/test_kernel_transport.c" \
	-o "$build_dir/test_kernel_transport"
"$build_dir/test_kernel_transport" --unit

echo "quality: kernel transport property tests"
"$build_dir/test_kernel_transport" --property

echo "quality: kernel transport address and undefined behavior sanitizers"
$cc $kernel_common -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined "$quality_dir/test_kernel_transport.c" \
	-o "$build_dir/test_kernel_transport_san"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/test_kernel_transport_san"

echo "quality: kernel SPSC ThreadSanitizer stress"
$cc $kernel_common -O1 -g -fno-omit-frame-pointer -pthread \
	-fsanitize=thread "$quality_dir/test_kernel_transport_threads.c" \
	-o "$build_dir/test_kernel_transport_tsan"
TSAN_OPTIONS=halt_on_error=1 "$build_dir/test_kernel_transport_tsan"

echo "quality: kernel transport coverage guided fuzzing ($fuzz_runs runs)"
$cc $kernel_common -O1 -g -fno-omit-frame-pointer \
	-fsanitize=fuzzer,address,undefined \
	"$quality_dir/fuzz_kernel_transport.c" \
	-o "$build_dir/fuzz_kernel_transport"
kernel_fuzz_log="$build_dir/kernel-fuzz.log"
if ! ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1 \
	"$build_dir/fuzz_kernel_transport" -runs="$fuzz_runs" -max_len=1024 \
	-artifact_prefix="$build_dir/kernel-" >"$kernel_fuzz_log" 2>&1
then
	tail -200 "$kernel_fuzz_log" >&2
	exit 1
fi

cc_major=$($cc -dumpversion | sed 's/\..*//')
llvm_cov=llvm-cov-$cc_major
llvm_profdata=llvm-profdata-$cc_major
if ! command -v "$llvm_cov" >/dev/null 2>&1; then
	llvm_cov=llvm-cov
fi
if ! command -v "$llvm_profdata" >/dev/null 2>&1; then
	llvm_profdata=llvm-profdata
fi
if ! command -v "$llvm_cov" >/dev/null 2>&1 ||
   ! command -v "$llvm_profdata" >/dev/null 2>&1
then
	echo "quality: LLVM coverage tools are required for CRAP measurement" >&2
	exit 1
fi

echo "quality: per-function CRAP measurement"
profile_dir="$build_dir/coverage"
mkdir -p "$profile_dir"
$cc $common -O0 -g -fprofile-instr-generate -fcoverage-mapping \
	"$source_dir/lifecycle.c" "$source_dir/test_lifecycle.c" \
	-o "$profile_dir/lifecycle"
LLVM_PROFILE_FILE="$profile_dir/lifecycle.profraw" \
	"$profile_dir/lifecycle" >/dev/null
"$llvm_profdata" merge -sparse "$profile_dir/lifecycle.profraw" \
	-o "$profile_dir/lifecycle.profdata"
"$llvm_cov" export "$profile_dir/lifecycle" \
	-instr-profile="$profile_dir/lifecycle.profdata" \
	>"$profile_dir/lifecycle.json"

$cc $kernel_common -O0 -g -fprofile-instr-generate -fcoverage-mapping \
	"$quality_dir/test_kernel_transport.c" \
	-o "$profile_dir/kernel-transport"
LLVM_PROFILE_FILE="$profile_dir/kernel-transport.profraw" \
	"$profile_dir/kernel-transport" >/dev/null
"$llvm_profdata" merge -sparse "$profile_dir/kernel-transport.profraw" \
	-o "$profile_dir/kernel-transport.profdata"
"$llvm_cov" export "$profile_dir/kernel-transport" \
	-instr-profile="$profile_dir/kernel-transport.profdata" \
	>"$profile_dir/kernel-transport.json"

python3 "$quality_dir/crap.py" --maximum 50 \
	--source "$source_dir/lifecycle.c" \
	--source "$include_dir/9ring.h" \
	--source "$include_dir/virtio9p_core.h" \
	--output "$profile_dir/crap.json" \
	"$profile_dir/lifecycle.json" "$profile_dir/kernel-transport.json"

mutation_dir=$(mktemp -d "$build_dir/mutations.XXXXXX")
trap 'rm -rf "$mutation_dir"' EXIT HUP INT TERM

kill_mutant()
{
	name=$1
	from=$2
	to=$3
	mutant="$mutation_dir/$name.c"
	sed "s/$from/$to/" "$source_dir/lifecycle.c" >"$mutant"
	if cmp -s "$source_dir/lifecycle.c" "$mutant"; then
		echo "quality: mutation $name was not applied" >&2
		exit 1
	fi
	$cc $common -O0 "$mutant" "$source_dir/test_lifecycle.c" \
		-o "$mutation_dir/$name"
	if "$mutation_dir/$name" >/dev/null 2>&1; then
		echo "quality: surviving mutant: $name" >&2
		exit 1
	fi
	echo "quality: killed mutant $name"
}

echo "quality: semantic mutation tests"
kill_mutant stale-generation \
	'event->generation != lifecycle->generation' \
	'event->generation == lifecycle->generation'
kill_mutant restart-budget \
	'lifecycle->restarts >= lifecycle->policy.max_restarts' \
	'lifecycle->restarts > lifecycle->policy.max_restarts'
kill_mutant deadline-edge \
	'event->now_ms >= lifecycle->deadline_ms' \
	'event->now_ms > lifecycle->deadline_ms'

kill_header_mutant()
{
	name=$1
	header=$2
	from=$3
	to=$4
	headers="$mutation_dir/$name-headers"
	mkdir "$headers"
	cp "$include_dir/9ring.h" "$headers/9ring.h"
	cp "$include_dir/virtio9p_core.h" "$headers/virtio9p_core.h"
	sed "s|$from|$to|" "$include_dir/$header" >"$headers/$header.mutated"
	if cmp -s "$include_dir/$header" "$headers/$header.mutated"; then
		echo "quality: mutation $name was not applied" >&2
		exit 1
	fi
	mv "$headers/$header.mutated" "$headers/$header"
	$cc -std=c11 -Wall -Wextra -Werror -iquote"$headers" -O0 \
		"$quality_dir/test_kernel_transport.c" -o "$mutation_dir/$name"
	if "$mutation_dir/$name" --unit >/dev/null 2>&1; then
		echo "quality: surviving mutant: $name" >&2
		exit 1
	fi
	echo "quality: killed mutant $name"
}

echo "quality: kernel transport semantic mutation tests"
kill_header_mutant ring-full 9ring.h \
	'(head - tail) >= P9_RING_SIZE' '(head - tail) > P9_RING_SIZE'
kill_header_mutant ring-empty 9ring.h \
	'tail == head' 'tail != head'
kill_header_mutant wire-length virtio9p_core.h \
	'v9pget32(message) == length' 'v9pget32(message) != length'
kill_header_mutant queue-boundary virtio9p_core.h \
	'size <= offered / 2' 'size < offered / 2'
kill_header_mutant descriptor-cycle virtio9p_core.h \
	'next != head' 'next == head'

echo "ASPERA-NATIVE-QUALITY PASS"
