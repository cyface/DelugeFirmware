# Plaits drum models

The files in this directory are ports of the drum synthesis models from
[Mutable Instruments Plaits](https://github.com/pichenettes/eurorack/tree/master/plaits)
by Emilie Gillet, together with the small subset of
[stmlib](https://github.com/pichenettes/stmlib) they depend on. All of that code is
MIT-licensed; each ported file keeps its original copyright header.

| File | Upstream source |
| --- | --- |
| `analog_bass_drum.h` | `plaits/dsp/drums/analog_bass_drum.h` (808 bass drum) |
| `analog_snare_drum.h` | `plaits/dsp/drums/analog_snare_drum.h` (808 snare drum) |
| `hi_hat.h` | `plaits/dsp/drums/hi_hat.h` (808 hi-hat and the ring-modulated "Hi-hat 2" variant) |
| `oscillator.h` | `plaits/dsp/oscillator/oscillator.h` + `stmlib/dsp/polyblep.h`, saw/square shapes only |
| `synthetic_bass_drum.h` | `plaits/dsp/drums/synthetic_bass_drum.h` (909-style bass drum) |
| `synthetic_snare_drum.h` | `plaits/dsp/drums/synthetic_snare_drum.h` (909-style snare drum) |
| `overdrive.h` | `plaits/dsp/fx/overdrive.h` |
| `dsp.h` | `plaits/dsp/dsp.h` + `plaits/dsp/oscillator/sine_oscillator.h` |
| `stmlib.h` | `stmlib/dsp/{dsp,filter,parameter_interpolator,units}.h`, `stmlib/utils/random.h` |

Changes from upstream:

- `kSampleRate` is 44100 (the Deluge's rate) instead of 48000. Every time constant in the
  models is written as seconds × `kSampleRate` and every frequency as cycles per sample, so
  nothing else needed re-deriving; the `FREQUENCY_FAST` tangent approximation was tuned for
  48 kHz but the error at 44.1 kHz is well under a cent across the drums' range.
- `SemitonesToRatio` uses `exp2f` instead of stmlib's LUT pair (it runs once per block, not
  per sample); `Sqrt` uses `sqrtf`; the sine LUT is filled at runtime by `initSineTable()`
  instead of shipping `resources.cc`.
- The classes are trivially constructible so one `DrumVoice` can hold all five models in a
  union and `Init()` only the one in use.
- Plaits' `voice.cc` trigger / decay-envelope / LPG wrapper is replaced by `DrumVoice`
  (`drum_voice.h`), which maps the Deluge's velocity to the models' accent input, arms the
  trigger on note-on, and reports when the drum has decayed to silence so the voice can be
  released. The Deluge's own envelopes and filters apply on top.

The parameter mapping from the three Plaits macros (HARMONICS / TIMBRE / MORPH) to each
model's arguments follows `plaits/dsp/engine/{bass_drum,snare_drum,hi_hat}_engine.cc`.
