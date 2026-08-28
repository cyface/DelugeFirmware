# Deluge plugin ABI (tier 1)

A versioned, plain-C boundary between the firmware (the **host**) and a DSP kernel (the **plugin**),
built so the same kernel source can be compiled straight into the firmware today and, later, as a
position-independent blob loaded from the SD card into SDRAM (the tier-2 loader proven by the #37
spike). Tier 1 is only the internal contract; nothing here loads code at runtime yet.

```
plugin/plugin_abi.h          the ABI: host API table, FX context, plugin descriptor (C, freestanding)
plugin/fx/<name>.{h,c}       kernels - plain C, no globals/statics/libc, firmware only via the API table
plugin/host/plugin_host.*    the firmware's API table + FxPluginSlot (owns an instance's state, on/off)
plugin/host/builtin_fx.*     descriptors for the kernels compiled into the firmware
plugin/check_freestanding.sh builds each kernel as a blob would be and fails on any contract breach
```

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

1. Write `plugin/fx/<name>.h` (state struct, param indices, `reset`/`render` prototypes) and
   `plugin/fx/<name>.c` against `plugin_abi.h`. Run `plugin/check_freestanding.sh`.
2. Add a descriptor to `plugin/host/builtin_fx.{h,cpp}` (with a `static_assert` against
   `kFxPluginMaxStateBytes`).
3. Give the owning `ModControllableAudio` an `FxPluginSlot` constructed with that descriptor, gather
   the raw q31 unpatched param values into an `int32_t[numParams]`, and call `slot.process()` from the
   FX chain with the enable flag and the insertion point's `levelShift`.

The param plumbing (enum, XML name, l10n strings, menus, automation lists) is still the firmware's
hand-written per-param work - see `docs/dev/tape_saturation.md` "Integration points". Generating that
from a plugin-declared param bank is the remaining tier-1 job.

## Reference plugin

`fx/tape_saturation.c` is the first kernel: the Tape saturation effect, moved here unchanged from
`ModControllableAudio` and verified bit-exact against the inline original (native harness, 3960
random blocks over all three drive bases, knob boundaries, block sizes 1/64/128, on/off toggling).
352 bytes of position-independent Thumb-2 as a blob.
