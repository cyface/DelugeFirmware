# Deluge plugin ABI (tier 1)

A versioned, plain-C boundary between the firmware (the **host**) and a DSP kernel (the **plugin**),
built so the same kernel source can be compiled straight into the firmware today and, later, as a
position-independent blob loaded from the SD card into SDRAM (the tier-2 loader proven by the #37
spike). Tier 1 is only the internal contract; nothing here loads code at runtime yet.

Two kinds of plugin exist: **insert FX** (`DelugeFxPlugin`, stereo in-place render at the end of the
distortion stage) and **source plugins** (`DelugeSourcePlugin`, a per-voice oscillator the Deluge plays
as `OscType::DRUM`).

```
plugin/plugin_abi.h            the ABI: host API table, FX context, param info, both plugin descriptors (C, freestanding)
plugin/fx/<name>.{h,c}         insert-FX kernels - plain C, no globals/statics/libc, firmware only via the API table
plugin/source/<name>.{h,c}     source kernels, same rules (plaits_drums = the Plaits drum models; README.md there)
plugin/host/builtin_fx.h       constexpr descriptors (incl. param tables) for the FX kernels compiled in
plugin/host/builtin_sources.h  constexpr descriptor (incl. model + macro-name tables) for the source kernel compiled in
plugin/host/fx_plugin_bank.h   the FX registry: assigns each plugin its slots in the shared param bank, and the
                               constexpr lookups everything else derives names/defaults from
plugin/host/plugin_host.*      the firmware's API table, FxPluginSlot / FxPluginChain, SourcePluginVoice
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

- **No globals, no statics, no libc.** All state lives in a host-owned block (`stateSize` /
  `voiceStateSize` bytes) and everything else comes in through the argument list. This is what leaves
  a blob with no relocations that need a load address and no GOT; `check_freestanding.sh` asserts it
  (no `.data`/`.bss`/`.got`, no undefined symbols, no absolute relocations - PC-relative calls between
  the kernel's own functions and references to its own `.rodata` are fine, a link fixes those to
  constant offsets).
- **Firmware only through `DelugePluginHostApi`.** Lookup tables and other firmware helpers are
  reached through function pointers, never linked. The spike measured one host call per sample as
  free once warm. Float kernels get `exp2f` and a sine table this way; anything else from libm is an
  import the check script will reject.
- **No integer division.** Cortex-A9 has no integer divide, so a `/` on integers silently imports
  `__aeabi_uidiv`/`__aeabi_uldivmod` from libgcc - the check script catches it; write a
  shift-subtract loop (see `udiv64by32` in `tape_saturation.c`) or move it to the host. Float
  division and `__builtin_sqrtf` are single VFP instructions and fine (the firmware and the check
  script both build with `-fno-math-errno`-equivalent flags so sqrt is never a call).
- **Float is allowed, but the toolchain flags are part of the ABI.** Both sides are
  `-mfloat-abi=hard -mfpu=neon`; samples, params and pitch still cross the boundary as integers.
- **No pointer tables in the kernel.** A `static const` array of function pointers or strings is a
  table of absolute addresses; descriptors with names and entry points live host-side (or, for a
  blob, get built by the loader from offsets in the file header). Small constant lookup tables of
  plain numbers are fine (GCC may put a `switch` in `.rodata`; that is a PC-relative reference).
- `(int32_t)(((int64_t)a * b) >> 32)` is the portable spelling of the firmware's
  `multiply_32x32_rshift32` (`smmul`); GCC emits the same instruction.
- FX reset semantics: the host calls `reset()` whenever the FX goes from off to on, then `render()`
  every block while it stays on. Off blocks never reach the kernel.
- Source voice semantics: `init(model, seed)` on a fresh voice, `trigger(accent)` per hit (also on a
  voice that is still ringing - a retrigger must not clear the resonators), `render()` every block
  until it returns 0, after which the host frees the voice. Each `render` gets `scratchSize` bytes of
  host-lent working memory valid for that call only (the drums put their float render buffer and the
  ring-mod hi-hat's two temporaries there rather than in the voice).

## Towards tier 2 (a blob loaded from the card) - what this pilot taught

The drums were ported with the loader in mind, so the remaining gap is known:

- **Descriptors must come from data, not code.** A blob cannot carry `kPlaitsDrums` as-is: its string
  and function pointers are absolute addresses. The file header needs the same fields as
  `DelugeSourcePlugin` with names as offsets into a string table and entry points as offsets into
  `.text`; the loader builds the descriptor the host already consumes. Nothing host-side needs to
  change for that, because every consumer (menus, XML, voice allocation) already reads the
  descriptor rather than an enum.
- **`.rodata` ships with the blob.** A real kernel has small constant tables (GCC's `switch` tables
  here); the image is `.text` + `.rodata` copied together, and the check script now distinguishes
  PC-relative relocations (fine) from absolute ones (not).
- **Float kernels need float services.** libm is not linkable from a blob, so `exp2f` and the sine
  table are host services; anything else a future kernel needs (`sinf`, `powf`, ...) goes the same
  way, and the hard-float calling convention is part of the ABI.
- **Noise must be seeded, not shared.** A global RNG is a `.bss` variable; per-voice state seeded by
  the host keeps hits varied and the blob relocation-free.
- **Scratch is a host loan.** Kernels that need more working memory than a voice should hold ask for
  it per call (`scratchSize`) instead of keeping `static` buffers.
- Still open: the loader itself (`PLUGINS/` scan, header parse, SDRAM placement with the L2
  clean+invalidate rules from the #37 spike), an ABI handshake that refuses a mismatched
  `abiVersion`, fault attribution and safe boot, and letting a loaded plugin register a new
  `OscType`/menu entry rather than only replacing the built-in drum plugin.

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

## Source plugins

A source plugin is one oscillator type with a list of **models** and up to three **macros**. The
Deluge shows the models as the `Osc* drum model` menu and saves the choice by the model's
`fileName` (`<osc1 type="drum" drumModel="808kick">`); the macros ride on the source's otherwise
unused patched params (pulse width, wave index, carrier feedback) so they are automatable and
patchable for free, and the menu items take their names from the plugin's per-model macro table
(a kick's third macro reads Drive, a snare's Snappy, a hi-hat's Noise). Nothing in the l10n files
names a model.

Per unison part the host allocates `voiceStateSize` bytes behind a `SourcePluginVoice` header
from the audio heap, exactly as it does a `DxVoice`. `Voice::renderBasicSource` converts the
patcher's final values to q31 macros, hands the note's phase increment over unchanged (q32 cycles
per sample is the kernel's pitch unit) and mixes the returned mono q31 block into the osc buffer
like a basic oscillator.

## Reference plugins

`source/plaits_drums.c` is the source-plugin pilot: the six Plaits drum models moved here from
`dsp/drums/` (C++) as plain C, bit-exact against the C++ they replace (native harness, all six
models, 96 000 random blocks, 0 mismatches). 8.9 KB of position-independent Thumb-2 plus 36 bytes
of `.rodata` as a blob.

`fx/tape_saturation.c` is the first kernel: the Tape saturation effect, moved here unchanged from
`ModControllableAudio` and verified bit-exact against the inline original (native harness, 3960
random blocks over all three drive bases, knob boundaries, block sizes 1/64/128, on/off toggling).
352 bytes of position-independent Thumb-2 as a blob.
