# #37 plugin-architecture spike: runtime-loaded code executing from SDRAM

**Result (2026-08-27, Deluge OLED hardware, release build `sdram-plugin-spike`):** a position-independent
plugin blob dropped into `PLUGINS/spike.dlp` on the SD card is read at runtime into SDRAM, validated,
executed, and renders **bit-exactly** the same output as the identical kernel linked into internal RAM,
calling back into the firmware through an API table on every sample, at **the same warm cost**.

| variant | code address | cold_min | cold_med | warm_min | warm_med | checksum | host calls / block |
|---|---|---|---|---|---|---|---|
| internal (linked `.text`) | `0x200637E9` | 5,527 | 7,708 | 6,333 | 7,258 | `0x402A0AD9` | 128 |
| blob, embedded → SDRAM | `0x0C032A11` | 8,968 | 9,004 | 6,339 | 7,258 | `0x402A0AD9` | 128 |
| blob, `PLUGINS/spike.dlp` → SDRAM | `0x0C03AE31` | 8,939 | 8,966 | 6,338 | 7,258 | `0x402A0AD9` | 128 |

Cycles (PMU `PMCCNTR`) per 128-sample block of a 2-saw + one-pole-LP kernel with one host call per
sample, 9 trials, min/median. "Cold" = every cache cleaned+invalidated before the call, i.e. the worst
case a plugin could see between audio blocks; ~3.4k cycles ≈ 8.5 µs at 400 MHz, ~0.3% of a 2.9 ms
block. Warm placement invariance matches PR #4764's linked-`.sdram_text` numbers (its 33 KB "sprawl"
kernel: 4.9× cold penalty, identical warm).

## What is in here

- `spike_plugin.h` — the ABI: 16-byte `DlpHeader` (`"DLP1"`, abi, entry offset), `SpikeHostApi` function
  table (the plugin's only way to call the firmware), state/params structs, render signature.
- `spike_plugin.c` — the kernel. Compiled twice from the same source: into the blob and into the firmware
  (`io/debug/plugin_spike.cpp` includes it) so the two are compared bit-for-bit.
- `blob_main.c`, `plugin.ld`, `build.sh` — the blob build: same target flags as the firmware
  (`-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb`), `-fPIC -mpic-data-is-text-relative`,
  freestanding, own linker script with the header at 0 and the entry at 16. The link **asserts** no
  `.data`, no `.bss`, no `.got`, and `build.sh` prints undefined symbols and relocations (both empty).
  Outputs `spike.dlp` (132 bytes) and `spike_blob.h`.
- `push_file.py` — writes a local file to the card over USB MIDI via the firmware's JSON sysex file
  protocol (`storage/smsysex.cpp`; creates missing directories). No card removal needed.
- `console.py` — attaches the sysex debug console and prints the firmware's report lines.
- `oled_dump.py` — grabs the OLED framebuffer over sysex and renders a PNG (useful when the person at
  the device and the person at the terminal are not the same).

Firmware side (`src/deluge/io/debug/plugin_spike.{h,cpp}`): on sysex-console attach it runs the reference,
copies the embedded blob into an SDRAM buffer, reads `PLUGINS/spike.dlp` into another, makes each visible
to instruction fetch (`invalidate_range_all_caches` = clean+invalidate D and L2 by range, then
`L1_I_CacheFlushAll`, `BPIALL`, `dsb; isb`), validates the header, calls `base + entryOffset | 1`, and
reports JSON lines one per scheduler tick.

## Run it

```bash
./contrib/plugin_spike/build.sh                                   # -> spike.dlp, spike_blob.h
./dbt build release && <flash it>                                  # deluge-flash skill or dbt loadfw
DBT_NO_SYNC=1 ./dbt exec 'python3 contrib/plugin_spike/push_file.py contrib/plugin_spike/spike.dlp PLUGINS/spike.dlp'
DBT_NO_SYNC=1 ./dbt exec 'python3 contrib/plugin_spike/console.py 20'
```

## What the spike settled for the plugin design (#37)

1. **Executing runtime-loaded code from SDRAM works** with the cache maintenance above and costs nothing
   warm. Paul's 2023 "maybe timing?" was the missing maintenance (root-caused in upstream PR #4764, whose
   four enabling commits this branch cherry-picks; upstream's `performance/ui_to_sdram` is doing the same).
2. **A GOT-free, relocation-free blob is achievable with plain GCC flags** as long as the plugin has no
   globals/statics and reaches the firmware only through the API table (the Korg logue model). That is the
   Tier 2 loader's contract; the linker-script asserts are how the SDK enforces it.
3. **Cross-calls in both directions are fine**: Thumb-2 blob → internal-RAM host function via `blx` through
   the table, host → blob entry via a plain function pointer with the Thumb bit.
4. **Cold penalty is per cache line and small at plugin sizes** (~145 cycles/32 B line): an 8 KB render
   loop fully cold costs ~37k cycles ≈ 90 µs, ~3% of a block, worst case; in practice the loop stays warm.
5. **Two hardware gotchas found on the way, both fixed on this branch:** the chainloader left stale L2
   lines (any non-identical `dbt loadfw` image froze — `chainload-l2-cold`), and the PR's benchmark
   invalidated L2 without cleaning it (heap corruption `M000`/`S002` a minute later — fixed to
   clean+invalidate). Rule: on this firmware L2 holds dirty data, never invalidate it without cleaning.

Not covered (next for Tier 2): a real render-context ABI (params, voice state ownership, block size),
loading multiple plugins and a PLUGINS/ directory scan, a version/ABI handshake, fault attribution, a
safe-boot key, and `.data`/`.bss` support if the SDK ever needs it (would need a tiny relocation pass).
