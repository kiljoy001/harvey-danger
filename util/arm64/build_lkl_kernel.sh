#!/bin/sh
# Build lkl.o for the Harvey aarch64 LKL driver families.
#
# Follows the recipe in doc/drivers/lkl.md, then merges the USB fragment.
# The system clang (22) cannot emit AArch64, so LLVM 23 supplies the
# compiler and the system ld.lld supplies the linker.
#
#   ./util/arm64/build_lkl_kernel.sh [build-dir] [install-dir]
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
lkl=${LKL_SRC:-$HOME/Repo/lkl}
build=${1:-/tmp/opencode/lkl-build}
install=${2:-/tmp/opencode/lkl-install}
llvm=${LLVM_BIN:-/usr/lib/llvm-23/bin}
tools=$build/tools

mkdir -p "$build" "$install" "$tools"
for t in clang llvm-ar llvm-nm llvm-objcopy llvm-objdump llvm-strip; do
	[ -x "$llvm/$t" ] && ln -sf "$llvm/$t" "$tools/$t"
done
ln -sf "$(command -v ld.lld)" "$tools/ld.lld"

make -C "$lkl" ARCH=lkl O="$build" LLVM="$tools/" \
	CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu defconfig

# defconfig detects the host object format; override it for the target.
"$lkl/scripts/config" --file "$build/.config" \
	--set-str OUTPUT_FORMAT elf64-littleaarch64

# Merge the family fragments (USB now; eMMC is already native).
for frag in "$root"/util/arm64/lkl-*.config; do
	[ -f "$frag" ] || continue
	"$lkl/scripts/kconfig/merge_config.sh" -m -O "$build" \
		"$build/.config" "$frag"
done

make -C "$lkl" ARCH=lkl O="$build" LLVM="$tools/" \
	CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu \
	LD="$tools/ld.lld -m aarch64elf" olddefconfig

make -C "$lkl" ARCH=lkl O="$build" LLVM="$tools/" \
	CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu \
	LD="$tools/ld.lld -m aarch64elf" -j"$(nproc)"

make -C "$lkl" ARCH=lkl O="$build" LLVM="$tools/" \
	CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu \
	LD="$tools/ld.lld -m aarch64elf" \
	INSTALL_PATH="$install" install

echo "lkl.o: $install/lib/lkl.o"
