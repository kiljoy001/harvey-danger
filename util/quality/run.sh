#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
source_dir="$root/sys/src/cmd/o9drvman"
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

echo "ASPERA-NATIVE-QUALITY PASS"
