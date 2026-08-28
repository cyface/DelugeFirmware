# Deluge plugin ABI (tier 1)

A versioned, plain-C boundary between the firmware (the **host**) and a DSP kernel (the **plugin**),
built so the same kernel source can be compiled straight into the firmware today and, later, as a
position-independent blob loaded from the SD card into SDRAM (the tier-2 loader proven by the #37
spike). Tier 1 is only the internal contract; nothing here loads code at runtime yet.

```
plugin/plugin_abi.h            the ABI: host API table, FX context, param info, plugin descriptor (C, freestanding)
plugin/fx/<name>.{h,c}         kernels - plain C, no globals/statics/libc, firmware only via the API table
plugin/host/builtin_fx.h       constexpr descriptors (incl. param tables) for the kernels compiled in
plugin/host/fx_plugin_bank.h   the registry: assigns each plugin its slots in the shared param bank, and the
                               constexpr lookups everything else derives names/defaults from
plugin/host/plugin_host.*      the firmware's API table, FxPluginSlot (state, on/off) and FxPluginChain
plugin/check_freestanding.sh   builds each kernel as a blob would be and fails on any contract breach
```

## The param bank

The firmware reserves `params::kNumFxPluginParams` shared unpatched params
(`UNPATCHED_FX_PLUGIN_PARAM_FIRST` onwards) and `fx_plugin_bank.h` hands them out, in registry order, to
the built-in plugins. A plugin declares its params once, as `DelugeFxParamInfo` (OLED name, 7-segment
name, XML attribute, default), and from that single table the firmware derives:

| formerly a hand edit in | now |
|---|---|
| `param.cpp` display name table | `getParamDisplayName()` asks the bank |
| `param.cpp` `paramNameForFile` / `fileStringToParam` | bank lookup (constexpr, so the round-trip `static_assert` still covers it) |
| `mod_controllable_audio.cpp` defaults, XML write, XML read | loops over the bank |
| `menus.cpp` menu item + three Distortion menus | `FxPluginParam` items, one per slot, appended to every Distortion menu |
| `automation_view.cpp` both lists + counts | bank entries generated into the lists |
| `strings.h` / `english.json` / `seven_segment.json` | nothing - names come from the plugin |

Because the params live in the ordinary unpatched set, automation, MIDI learn, mod-knob assignment
and preset/song saving all work as for any other param. XML stays compatible: the attribute is the
plugin's `fileName`, so a song saved before the bank existed (`tapeSaturation="..."`) reads back
unchanged.

The plugin also gets an optional `isActive(params)` so it can declare when it is off (tape: knob at
minimum); the host bypasses it then and it costs nothing.

## The contract a kernel must keep

- **No globals, no statics, no libc.** All state lives in a host-owned block (`stateSize` bytes) and
  everything else comes in through the argument list. This is what leaves a blob with no
  relocations and no GOT; `check_freestanding.sh` asserts it (no `.data`/`.bss`/`.got`, no undefined
  symbols, no relocations).
- **Firmware only through `DelugePluginHostApi`.** Lookup tables and other firmware helpers are
  reached through function pointers, never linked. The spike measured one host call per sample as
  free once warm.
- **No division, no floating point in the kernel.** Cortex-A9 has no integer divide, so a `/` silently
  imports `__aeabi_uidiv`/`__aeabi_uldivmod` from libgcc - the check script catches it; write a
  shift-subtract loop (see `udiv64by32` in `tape_saturation.c`) or move it to the host.
- `(int32_t)(((int64_t)a * b) >> 32)` is the portable spelling of the firmware's
  `multiply_32x32_rshift32` (`smmul`); GCC emits the same instruction.
- Reset semantics: the host calls `reset()` whenever the FX goes from off to on, then `render()`
  every block while it stays on. Off blocks never reach the kernel.

## Adding an insert FX

1. Write `plugin/fx/<name>.h` (state struct, param indices, `reset`/`isActive`/`render` prototypes) and
   `plugin/fx/<name>.c` against `plugin_abi.h`. Run `plugin/check_freestanding.sh`.
2. Describe it in `plugin/host/builtin_fx.h`: a constexpr `DelugeFxParamInfo[]` (names, XML attribute,
   default per param) and the `DelugeFxPlugin` descriptor.
3. Append it to `kBuiltinFxPlugins` in `plugin/host/fx_plugin_bank.h` and bump
   `params::kNumFxPluginParams` in `param.h` by its param count (a `static_assert` tells you if they
   disagree).

That is all: it renders at the end of the distortion stage on synths, kits, audio clips and the song
master, its knobs appear in every Distortion menu and in the automation view, and its params save and
load by name. Nothing to touch in `param.cpp`, `menus.cpp`, `automation_view.cpp`, the l10n files or
`ModControllableAudio`.

## Reference plugin

`fx/tape_saturation.c` is the first kernel: the Tape saturation effect, moved here unchanged from
`ModControllableAudio` and verified bit-exact against the inline original (native harness, 3960
random blocks over all three drive bases, knob boundaries, block sizes 1/64/128, on/off toggling).
352 bytes of position-independent Thumb-2 as a blob.
