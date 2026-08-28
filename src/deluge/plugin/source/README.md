# Plaits drum models (source plugin kernel)

`plaits_drums.c` is a plain-C port of the drum synthesis models from
[Mutable Instruments Plaits](https://github.com/pichenettes/eurorack/tree/master/plaits)
by Emilie Gillet, together with the small subset of
[stmlib](https://github.com/pichenettes/stmlib) they depend on, written against the tier-1
plugin ABI (`plugin/plugin_abi.h`, `DelugeSourcePlugin`). All of that code is MIT-licensed; the
notice is kept at the top of the file.

| Section of `plaits_drums.c` | Upstream source |
| --- | --- |
| filters, `pd_tan_*`, interpolator, soft clip, random | `stmlib/dsp/{dsp,filter,parameter_interpolator,units}.h`, `stmlib/utils/random.h` |
| PolyBLEP oscillator | `plaits/dsp/oscillator/oscillator.h` + `stmlib/dsp/polyblep.h`, saw/square only |
| overdrive | `plaits/dsp/fx/overdrive.h` |
| 808 bass drum | `plaits/dsp/drums/analog_bass_drum.h` |
| 808 snare drum | `plaits/dsp/drums/analog_snare_drum.h` |
| hi-hats | `plaits/dsp/drums/hi_hat.h` (the 808 hi-hat and the ring-modulated "Hi-hat 2" variant) |
| 909 bass drum | `plaits/dsp/drums/synthetic_bass_drum.h` |
| 909 snare drum | `plaits/dsp/drums/synthetic_snare_drum.h` |
| macro mapping | `plaits/dsp/engine/{bass_drum,snare_drum,hi_hat}_engine.cc` |

## Changes from upstream

- `kSampleRate` is 44100 (the Deluge's rate) instead of 48000. Every time constant in the models
  is written as seconds × sample rate and every frequency as cycles per sample, so nothing else
  needed re-deriving; the `FREQUENCY_FAST` tangent approximation was tuned for 48 kHz but the error
  at 44.1 kHz is well under a cent across the drums' range. The ring-mod hi-hat evaluates its pair
  frequencies at the 48 kHz scale so its partials land where Plaits puts them.
- C++ templates and classes became structs and functions; the `HiHat<...>` template's two
  instantiations are one function with a `ringMod` flag.
- The sine table and `exp2f` (`SemitonesToRatio`) come from the host through the API table; the
  LCG noise source (`stmlib::Random`) lives in the voice and is seeded by the host on every init,
  so a kernel has no globals and links to nothing.
- Plaits' `voice.cc` trigger / decay-envelope / LPG wrapper is replaced by the voice bookkeeping in
  `plaits_drums_render`: the host's accent (from velocity) drives the models' accent input, a
  trigger is armed by `plaits_drums_trigger`, and the kernel reports when the drum has decayed to
  silence (four consecutive blocks under about -86 dBFS) so the host can release the voice. The
  Deluge's own envelopes and filters apply on top.
- The models' `sustain` (free-running) mode is not ported: the Deluge treats a drum as a one-shot.
- Kicks play two octaves and snares one octave below the note so a kit row at C3 lands in the
  classic register (`pd_model_pitch_scale`).

The port is bit-exact against the C++ version it replaces (`dsp/drums/` before this change): a
native harness ran all six models over 96 000 blocks of random macros, pitches, accents, block
sizes and mid-ring retriggers with zero sample mismatches.
