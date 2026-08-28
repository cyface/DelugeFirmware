#!/usr/bin/env bash
# Prove a plugin kernel honours the tier-1 contract by building it the way the tier-2 loader needs it:
# freestanding, position-independent, no libc - and asserting the result has no .data, no .bss, no GOT, no
# undefined symbols and no relocation that needs a load address (PC-relative calls between the kernel's own
# functions and PC-relative references to its own .rodata are fine: a link fixes them to constant offsets, and the
# blob is .text + .rodata copied as one image). Same target flags as the firmware and as the #37 spike blob: they
# come from pack_dlp.py, so the object here and the blob on the card are built identically.
#
# Then the tier-2 half: every built-in plugin is packed into a .dlp and read back through the host's own parser,
# and a corrupted copy of each is checked to be rejected for the right reason (pack_dlp.py --self-test).
#
#   src/deluge/plugin/check_freestanding.sh [kernel.c ...]     (default: every plugin/fx/*.c and plugin/source/*.c)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(git -C "$HERE" rev-parse --show-toplevel)"
BIN="$ROOT/toolchain/current/arm-none-eabi-gcc/bin"
read -ra FLAGS <<<"$(python3 "$HERE/tools/pack_dlp.py" --print-flags)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

kernels=("$@")
[ ${#kernels[@]} -gt 0 ] || kernels=("$HERE"/fx/*.c "$HERE"/source/*.c)

status=0
for src in "${kernels[@]}"; do
	name="$(basename "${src%.c}")"
	obj="$OUT/$name.o"
	"$BIN/arm-none-eabi-gcc" "${FLAGS[@]}" -c "$src" -o "$obj"
	fail=""
	undef="$("$BIN/arm-none-eabi-nm" -u "$obj" || true)"
	[ -z "$undef" ] || fail+=$'\n  undefined symbols:\n'"$undef"
	pcrel='R_ARM_(THM_)?(CALL|JUMP24|JUMP19|PC8|PC11|PC12|PC13|PC22|MOVW_PREL_NC|MOVT_PREL)|R_ARM_(REL32|PREL31|LDR_PC_G0)'
	relocs="$("$BIN/arm-none-eabi-readelf" -r "$obj" | grep -E '^[0-9a-f]{8} ' | grep -Ev "$pcrel" || true)"
	[ -z "$relocs" ] || fail+=$'\n  relocations needing a load address:\n'"$relocs"
	sections="$("$BIN/arm-none-eabi-size" -A "$obj" | awk '$1 ~ /^\.(data|bss|got)/ && $2 > 0 {print "  " $1 " " $2 " bytes"}')"
	[ -z "$sections" ] || fail+=$'\n  writable/GOT sections:\n'"$sections"
	text="$("$BIN/arm-none-eabi-size" -A "$obj" | awk '$1 ~ /^\.text/ {t += $2} END {print t}')"
	if [ -z "$fail" ]; then
		rodata="$("$BIN/arm-none-eabi-size" -A "$obj" | awk '$1 ~ /^\.rodata/ {t += $2} END {print t + 0}')"
		echo "OK   $name: ${text} bytes of position-independent code + ${rodata} bytes rodata, no data/bss/got, no absolute relocations, no imports"
	else
		echo "FAIL $name:$fail"
		status=1
	fi
done

# The blob the loader will actually meet: pack every built-in, read it back through plugin_blob.h, and check that
# a damaged one is refused. Skipped (with a note) if the kernels above already failed.
if [ $status -eq 0 ]; then
	python3 "$HERE/tools/pack_dlp.py" --all --self-test --out "$OUT/plugins" || status=1
else
	echo "     (skipping the .dlp round trip: fix the kernel above first)"
fi
exit $status
