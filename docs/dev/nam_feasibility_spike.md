# NAM on the Deluge — feasibility spike

*Research spike, August 16, 2026. What it would take to run the Neural Amp Modeler (NAM)
inference engine on the Deluge — as a guitar amp sim on the line input, and as an insert on
the master mix. All firmware `file:line` references are against this repo; estimates are
flagged as such throughout.*

## Verdict

**Feasible, with the CPU as the whole story.** Memory, toolchain, licensing, and integration
points are all clean — the only real question is whether inference fits in the cycles the
synth engine can spare on the single 400 MHz Cortex-A9.

| Option | Assessment | Est. CPU share |
|---|---|---|
| **NAM A2-Lite** | Stretch goal — the new embedded-targeted architecture, full TONE3000 capture library | ~33–55% |
| **NAM A1-Nano** | Realistic target — smallest previous-gen preset, ~2.6× cheaper than A2-Lite | ~15–30% |
| **RTNeural LSTM** | Safety net — the AIDA-X / Proteus approach; different capture ecosystem, lower fidelity | ~11–28% (LSTM-16) |

A guitar amp on the line input while playing a moderate song is plausible; running a dense
12-voice project through NAM on the master bus is probably not, on this silicon.

**The decisive next step is cheap:** a 1–2 day on-device benchmark (Phase 0 below) that
replaces every estimate in this document with a measured cycles-per-sample number.

## The budget

The Deluge runs a single Cortex-A9 at 400 MHz (`src/RZA1/peripheral_init_basic.c:96-108`)
with audio at 44.1 kHz (`src/definitions_cxx.hpp:1035`). That is **~9,070 CPU cycles per
sample for everything** — synthesis, sequencer, UI, SD streaming, and any new effect.

**Calibration anchor:** the optimized bare-metal RP2350 port measured A2-Lite at
**8,396 cycles/sample** on a scalar Cortex-M33. The A9's out-of-order core plus NEON should
land meaningfully below that, but nobody has published neural-inference numbers for the
RZ/A1L specifically.

Two constraints sharpen the budget:

1. **A9 NEON is the weak early version** — no fused multiply-add, a 64-bit floating-point
   datapath — so realistic hand-tuned throughput is on the order of 0.2 GMAC/s at 400 MHz,
   not the theoretical peak.
2. **The firmware already defends against overload by killing voices**: `setDireness()`
   (`src/deluge/processing/engines/audio_engine.cpp:503-547`) converts the audio task's
   average runtime into a load score and `cullVoices()` reaps polyphony when it climbs.
   A fixed per-sample NAM tax converts directly into fewer simultaneous voices.

## What "A2-Lite" actually is

Architecture 2 launched June 2026 (TONE3000 + Steven Atkinson). Still a dilated-causal-conv
WaveNet, but redesigned for embedded use: a single 23-layer array with a hand-tuned dilation
schedule, LeakyReLU instead of tanh, receptive field ~6,300 samples. Exactly two sizes exist —
**A2-Full** (8 channels, DAW/pro hardware) and **A2-Lite** (3 channels, ~1,871 parameters
≈ 7.5 KB of float32 weights). There is nothing smaller in the A2 line; "A2-Nano" in early
materials was A2-Lite's pre-launch name. TONE3000's reference point: A2-Lite at **50% CPU on
a 600 MHz Cortex-M7**.

"Packed training" means one training run yields a model file containing both sizes, and a
compact binary format (`.namb`) exists specifically for embedded loaders. The whole A2 stack
is MIT-licensed.

| Model | Shape | ~MACs/sample¹ | Embedded proof point |
|---|---|---|---|
| A2-Full | 23 layers × 8 ch | ~6,300 | Darkglass Anagram, Dimehead (Linux-class SoCs) |
| **A2-Lite** | 23 layers × 3 ch | ~1,900 | Daisy Seed (M7 480), RP2350 dual-core, Teensy 4, Hotone/Mooer/NUX pedals |
| A1-Standard | 2 arrays, 16/8 ch | ~13,300 | Desktop / Dimehead — impossible here |
| A1-Feather | 2 arrays, 8/4 ch | ~2,800 | — |
| **A1-Nano** | 2 arrays, 4/2 ch | ~730 | Original Daisy Seed target |
| LSTM-16 (AIDA-X) | 1 layer, hidden 16 | ~1,200 | MOD Dwarf; Pi 4 ran LSTM-40 at ~50% of one core |

¹ Derived from the published layer configs, not official figures. Measured cost ratios
flatten at small sizes because per-layer overhead dominates.

## Where it plugs into the firmware

### Line-in guitar amp (the primary use case)

The clean insert point already exists. An audio track (`AudioOutput`) in **looper/FX mode**
monitors the line input through a full block-based effect chain: the live input is mixed into
the render buffer at `src/deluge/processing/audio_output.cpp:211-261`, then flows through
saturation → filters → mod FX/EQ/delay → stutter → reverb send → compressor
(`src/deluge/model/global_effectable/global_effectable_for_clip.cpp:49-172`).

A NAM stage slots into that chain exactly the way the **GRAIN** granular effect did: a new
`ModFXType` enum entry (`src/definitions_cxx.hpp:409-419`), a lazily-allocated processor
object (`global_effectable.cpp:1163-1177`, `mod_controllable_audio.cpp:1742-1763`), menu
wiring in `src/deluge/gui/ui/menus.cpp`, and reuse of the existing mod-FX knob params.
Monitoring latency is unchanged — WaveNet is causal, so the model adds zero algorithmic
latency.

### Master-bus insert (the secondary use case)

The whole mix is assembled in `renderSongFX()` (`audio_engine.cpp:896-945`), which already
runs song filters, stutter, pan, and the master compressor over the final buffer. A NAM hook
goes right beside the compressor at `audio_engine.cpp:930-942`. Same code, different
attachment point — but this is where the CPU math bites, because it stacks on top of full
song rendering.

### Things the engine gives us for free

- **NEON and float are first-class.** The build targets `-mfpu=neon -mfloat-abi=hard` with
  `-funsafe-math-optimizations` (`scripts/cmake/CMakeToolchainDeluge.cmake:56-73`); the
  Mutable-derived reverbs are pure float; the `argon` NEON C++ wrapper is already vendored;
  there is hand-written NEON asm precedent (`src/deluge/dsp/dx/neon_fm_kernel.s`).
- **Memory is a non-issue.** A2-Lite weights are ~7.5 KB. The internal-RAM heap has ~1.1 MB
  free, and the Mutable reverb already keeps a 128 KB working buffer in internal RAM as
  precedent. SDRAM has ~62 MB spare if needed (but it's 16-bit @ 66 MHz with L1-only data
  caching — hot inference state should stay internal).
- **Modern toolchain.** C++23, GCC 14.2, exceptions enabled. NeuralAmpModelerCore (MIT) +
  Eigen (MPL2) + RTNeural (BSD-3) are all compatible with the firmware's GPLv3.
- **Model loading path exists.** FatFS SD access plus the new `.namb` binary format (no JSON
  parser needed in the audio path); file-picker UI has plenty of precedent.

## The engineering obstacles

- **[High] CPU fit is unproven on this exact silicon.** No published neural-inference
  numbers exist for the RZ/A1L. The stock Eigen path is known-catastrophic on bare metal
  (it mallocs inside GEMM); the 2026 embedded work added `USE_NAM_INLINE_GEMM`, LUT
  activations, and the `.namb` loader specifically to fix this. Everything hinges on the
  Phase 0 measurement.
- **[Medium] Voice-culling feedback.** `setDireness()` can't tell NAM cost from voice cost,
  so a master-bus NAM will cause the engine to kill voices to "make room" for an effect that
  never gets cheaper. The load accounting needs to either budget NAM separately or accept
  reduced polyphony explicitly.
- **[Medium] Fixed-point ↔ float boundary and gain staging.** The engine is q31 end-to-end
  and the internal bus sits ~48 dB below full scale (`AUDIO_OUTPUT_GAIN_DOUBLINGS = 8`,
  `audio_engine.cpp:102`). NAM models expect roughly instrument-level float input, so the
  wrapper needs an input gain (a real "amp input trim" parameter anyway) and conversion each
  way — cheap with NEON, but it must be deliberate or every model will sound wrong.
- **[Medium] 44.1 kHz vs 48 kHz.** NAM captures are trained at 48 kHz. Running them at 44.1
  shifts the amp's frequency response down ~8% (about 1.5 semitones) — likely acceptable for
  a groovebox, but worth an A/B. The alternative, resampling 44.1→48→44.1 around the model,
  costs ~9% more inference plus resampler overhead.
- **[Low] Variable render blocks (1–128 samples).** The engine renders irregular windows,
  sometimes tiny (`audio_engine.cpp:583-614`). WaveNet inference handles arbitrary block
  sizes naturally; per-call overhead just argues for unrolled fixed-size kernels rather than
  generic GEMM.
- **[Low] Code size.** All code executes from internal RAM (execution from SDRAM doesn't
  work on this board), and `.text` + heap share the 3 MB. Eigen template instantiations and
  unrolled kernels will eat some of the ~1.1 MB heap headroom; needs watching, not fearing.

## Suggested plan

### Phase 0 — on-device benchmark (the decision gate)

Port NeuralAmpModelerCore + Eigen with `USE_NAM_INLINE_GEMM` and the `.namb` loader into the
firmware behind a dev hook. Feed a test buffer through A2-Lite and A1-Nano, time with
`TIMER_SYSTEM_SUPERFAST` (33.79 MHz, `src/definitions.h:36-49`), report cycles/sample over
RTT or sysex. Try Eigen-NEON vs. the inline kernels. This replaces every estimate on this
page with a fact, for a day or two of work.

### Phase 1 — line-in amp sim

- New effect following the GRAIN precedent: enum entry, lazily-allocated processor, menu
  wiring, community-feature toggle (same pattern as the sysex hot-reload work).
- Input gain + output level params mapped to the existing mod-FX gold-knob slots.
- Start with one hardcoded `.namb` model on SD; add a model-file picker once it makes sound.
- Weights + ring buffers in internal RAM via `allocMaxSpeed`.

### Phase 2 — master-bus insert

Same processor attached in `renderSongFX()` next to the master compressor, plus a decision
about the direness interaction (exempt NAM from the load score, or document the polyphony
cost). Only worth doing if Phase 0 lands in the optimistic half of the range.

### Phase 3 — optimization, as needed

Hand-NEON the 3-channel layer kernel with `argon` (3 channels + padding fits one NEON lane
group neatly), LUT the activation, consider
[mikeoliphant/NeuralAudio](https://github.com/mikeoliphant/NeuralAudio) (MIT) which ships
hand-optimized static implementations of exactly the A1/A2 configs, or fall back to an
RTNeural LSTM path.

## Honest bottom line

This is at the ragged edge of what a 2010-era 400 MHz core can do while also being a
groovebox. The Daisy Seed (480 MHz M7 doing nothing else) needed a year of community
optimization to reach real-time A2-Lite. The Deluge has a faster core architecture but must
share it with everything else. A1-Nano as a line-in amp is the bet to make; A2-Lite is worth
measuring before dismissing; whole-mix processing will always be a "light songs only" feature
on this hardware.

## Sources

- [TONE3000 — Introducing NAM Architecture 2](https://www.tone3000.com/blog/introducing-neural-amp-modeler-nam-architecture-2-a2)
  · [NAM A2: The Complete Guide](https://www.tone3000.com/guides/nam-a2-the-complete-guide)
  · [Running NAM on embedded hardware](https://www.tone3000.com/blog/running-nam-on-embedded-hardware)
- [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) (MIT)
  · [NeuralAudio](https://github.com/mikeoliphant/NeuralAudio)
  · [RTNeural](https://github.com/jatinchowdhury18/RTNeural)
  · [AIDA-X](https://github.com/AidaDSP/AIDA-X)
- [RP2350 bare-metal NAM port](https://github.com/oyama/pico-neural-amp-modeler-demo)
  (the 8,396 cycles/sample anchor)
  · [J.F. Santos — NAM A2 on embedded](https://jfsantos.dev/blog/nam-a2-embedded/)
- [gearnews — NAM hardware support guide (2026)](https://www.gearnews.com/neural-amp-modeler-guide-guitar/)
