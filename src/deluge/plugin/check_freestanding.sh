#!/usr/bin/env bash
# Prove a plugin kernel honours the tier-1 contract by building it the way the tier-2 loader would need it:
# freestanding, position-independent, no libc - and asserting the result has no .data, no .bss, no GOT, no
# undefined symbols and no relocations. Same target flags as the firmware and as the #37 spike blob.
#
#   src/deluge/plugin/check_freestanding.sh [kernel.c ...]     (default: every plugin/fx/*.c)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(git -C "$HERE" rev-parse --show-toplevel)"
BIN="$ROOT/toolchain/current/arm-none-eabi-gcc/bin"
FLAGS=(-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O2 -fPIC -mpic-data-is-text-relative
       -ffreestanding -nostdlib -fno-builtin -fno-exceptions -fno-asynchronous-unwind-tables
       -ffunction-sections -fdata-sections -fvisibility=hidden -Wall -Werror -std=gnu23
       -I "$ROOT/src/deluge")
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

kernels=("$@")
[ ${#kernels[@]} -gt 0 ] || kernels=("$HERE"/fx/*.c)

status=0
for src in "${kernels[@]}"; do
	name="$(basename "${src%.c}")"
	obj="$OUT/$name.o"
	"$BIN/arm-none-eabi-gcc" "${FLAGS[@]}" -c "$src" -o "$obj"
	fail=""
	undef="$("$BIN/arm-none-eabi-nm" -u "$obj" || true)"
	[ -z "$undef" ] || fail+=$'\n  undefined symbols:\n'"$undef"
	relocs="$("$BIN/arm-none-eabi-readelf" -r "$obj" | grep -E '^[0-9a-f]{8} ' || true)"
	[ -z "$relocs" ] || fail+=$'\n  relocations:\n'"$relocs"
	sections="$("$BIN/arm-none-eabi-size" -A "$obj" | awk '$1 ~ /^\.(data|bss|got)/ && $2 > 0 {print "  " $1 " " $2 " bytes"}')"
	[ -z "$sections" ] || fail+=$'\n  writable/GOT sections:\n'"$sections"
	text="$("$BIN/arm-none-eabi-size" -A "$obj" | awk '$1 ~ /^\.text/ {t += $2} END {print t}')"
	if [ -z "$fail" ]; then
		echo "OK   $name: ${text} bytes of position-independent code, no data/bss/got, no relocations, no imports"
	else
		echo "FAIL $name:$fail"
		status=1
	fi
done
exit $status
