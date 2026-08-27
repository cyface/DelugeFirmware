#!/usr/bin/env python3
"""Semantic diff of two preset files: python3 diff_presets.py OLD.XML NEW.XML

Parses both (element style or attribute style) into flat key/value maps and reports:
  * values present in both but different (the ones that matter),
  * keys only in OLD (dropped) and only in NEW (added - usually firmware defaults).
Hex values that differ by <= 64 LSB are reported as "grid-equivalent" (e.g. knob 50 is
0x7FFFFFD2 on the device but older generators wrote 0x7FFFFFFF). Exit status 1 if any
real difference remains.
"""

import sys

from _common import dp

# Old element-style writers used names the current serializer has replaced.
RENAMES = {"compressor": "sidechain"}
# Values that spell the same thing differently.
EQUIVALENT = {("polyphonic", "0", "poly"), ("polyphonic", "0", "auto")}


def load(path):
    flat = dp.flatten_preset_xml(path)
    out = {}
    for k, v in flat.items():
        for old, new in RENAMES.items():
            k = k.replace("/" + old + "/", "/" + new + "/")
        out[k] = v
    return out


def main(a_path, b_path):
    a, b = load(a_path), load(b_path)
    real, grid, equiv = [], [], []
    for k in sorted(set(a) & set(b)):
        va, vb = a[k], b[k]
        if va == vb:
            continue
        leaf = k.rsplit("/", 1)[-1]
        if (leaf, va, vb) in EQUIVALENT:
            equiv.append((k, va, vb))
        elif (
            va.startswith("0x")
            and vb.startswith("0x")
            and abs(dp.s32(va) - dp.s32(vb)) <= 64
        ):
            grid.append((k, va, vb))
        else:
            real.append((k, va, vb))
    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    print(
        f"{len(a)} keys in {a_path}, {len(b)} in {b_path}, {len(set(a) & set(b))} shared"
    )
    for k, va, vb in real:
        print(f"DIFF  {k}: {va} -> {vb}")
    for k, va, vb in grid:
        print(f"grid  {k}: {va} ~ {vb} (within 64 LSB)")
    for k, va, vb in equiv:
        print(f"same  {k}: {va} == {vb}")
    if only_a:
        print(
            f"only in OLD ({len(only_a)}): "
            + ", ".join(only_a[:12])
            + (" ..." if len(only_a) > 12 else "")
        )
    if only_b:
        # collapse per-row noise: show the distinct leaf names
        leaves = sorted({k.split("/", 1)[1] if "/" in k else k for k in only_b})
        print(
            f"only in NEW ({len(only_b)} keys, {len(leaves)} distinct): "
            + ", ".join(leaves[:40])
            + (" ..." if len(leaves) > 40 else "")
        )
    print(f"{len(real)} real differences")
    return 1 if real else 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
