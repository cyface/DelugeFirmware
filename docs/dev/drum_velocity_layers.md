# Drum Velocity Layers

Design notes for giving kit drums velocity-switched samples — the thing that separates a sampled
kit that sounds like a drummer from one that sounds like a drum machine.

**Status:** all three phases have landed, behind the `drumVelocityLayers` community feature (off by
default). See "What landed" at the end for the as-built notes.

## The problem

The firmware has no velocity layers anywhere. `MultiRange` carries exactly one key:

```cpp
class MultiRange {
public:
    virtual AudioFileHolder* getAudioFileHolder() = 0;
    int16_t topNote;
};
```

`MultiRangeArray` is an `OrderedResizeableArray` sorted on that single field, and
`Source::getRange()` does a `GREATER_OR_EQUAL` search on a note number. Velocity never enters
sample selection — it only reaches the patcher as a modulation source
(`voice.cpp`, `sourceValues[PatchSource::VELOCITY]`).

For pitched instruments you can paper over this with velocity → volume and velocity → filter
cutoff. For drums you can't: a soft snare is not a loud snare turned down, it's a different
spectrum. Layering two sample sets across the two oscillators does not work either — see
"Why not the two-oscillator trick" below.

## The opportunity

`SoundDrum::noteOn` passes a **constant** note code for every hit, from every trigger path:

```cpp
Sound::noteOn(modelStack, &arpeggiator, kNoteForDrum, mpeValues, sampleSyncLength,
              ticksLate, samplesLate, velocity, fromMIDIChannel);
```

with `constexpr int32_t kNoteForDrum = 60;` in `definitions_cxx.hpp`.

So inside a kit row the range array's sort key is **dead weight** — every lookup returns the same
element regardless of how many ranges exist. That key is free to repurpose.

This is what makes the change small: velocity (0–127) fits an `int16_t` with room to spare, the
ordered array needs no structural change, and the existing search semantics — ascending bounds,
last range open-ended — carry over to velocity unmodified.

Synths keep note-keyed ranges. Nothing about existing presets changes.

## Phase 1 — playback and serialization *(landed)*

This is the PR-able core. It is useful on its own: presets can be authored externally (a script
emitting the XML) with no on-device editing UI at all.

### Range selection

`voice.cpp`, in `Voice::noteOn`:

```cpp
MultiRange* range = sound.sources[s].getRange(noteCodeAfterArpeggiation + sound.transpose);
```

`velocity` is already a parameter of `Voice::noteOn` and in scope here, so this is a change at the
point of use, not a plumbing exercise. Gate it on a virtual added to `Sound`:

```cpp
virtual bool rangesKeyedByVelocity() const { return false; }
```

overridden in `SoundDrum` to return the runtime-feature state.

`Source::getRange()` itself needs **no change** — it takes an `int32_t` and the
`GREATER_OR_EQUAL` search plus the last-range clamp behave identically on a velocity key. Rename
its parameter from `note` to `key` for honesty.

### Serialization

Writer and reader both live in `sound.cpp` (search `rangeTopNote`). Emit `rangeTopVelocity` for
velocity-keyed sources and **accept both attributes on read**. Old files keep loading; new files
are self-describing rather than relying on context to disambiguate.

Two existing behaviours carry over unchanged and are both wanted:

- the **last** range omits its bound entirely, defaulting to `32767` — which covers velocity 127
- duplicate bounds are rejected as `FILE_CORRUPTED`, which is the right guard for velocity too

### Community feature toggle

Standard registration: enum in `runtime_feature_settings.h`, setup in `runtime_feature_settings.cpp`
`init()`, menu item in `gui/menu_item/runtime_feature/settings.cpp` (both the instance **and** the
`subMenuEntries` array), strings in `english.json` / `seven_segment.json` plus the enum in
`strings.h`. Seven-segment strings are capped at 4 characters. Never edit `g_english.cpp` or
`g_seven_segment.cpp` — they are regenerated during the build.

## Phase 2 — on-device editing *(landed)*

`gui/menu_item/multi_range.cpp` (~470 lines) is where most of the actual effort lives, and it is
note-oriented throughout: the range-bound nudging renders note names, and the range-splitting logic
computes note midpoints. `gui/menu_item/sample/transpose.h` also renders range bounds via
`noteCodeToString`.

All of it needs to show plain velocity numbers when the source is velocity-keyed. Worth keeping as
a separate change rather than bundling into the Phase 1 PR.

## Phase 3 — import flow *(landed)*

`SampleBrowser::importFolderAsKit()` currently creates one drum per file. A velocity-layer import
wants the opposite shape: one drum, N ranges, bounds distributed across velocity. New flow, new UI,
lowest priority — external preset authoring covers the need until then.

## Call sites to audit

Only four in the whole tree:

| Site | Note |
|---|---|
| `voice.cpp`, `Voice::noteOn` | the real one |
| `sound_editor.cpp` | editor's current-range tracking |
| `gui/menu_item/multi_range.cpp` | `getRangeIndex(noteCode)` |
| `gui/views/instrument_clip_view.cpp` | already drum-specific, passes `0`; decide whether it should pass a nominal velocity or keep returning range 0 for display purposes |

## Risks

**Memory is the constraint, not code.** Every range holds a resident sample with cluster reasons
claimed. A 9-drum kit at 4 velocity layers is 36 samples against the RZ/A1L's RAM. This should be
measured before choosing a recommended layer count — it is the difference between 4 layers being
comfortable and being the thing that makes the feature unusable on a full kit. Nothing in the code
survey answers this; it needs a real test on hardware or in the emulator.

Stereo samples double the SD streaming bandwidth per voice, which compounds with polyphony on a
busy kit.

## Why not the two-oscillator trick

Worth recording, because it looks like an obvious workaround and is not one. The idea: soft layer
on OSC1, loud layer on OSC2, crossfade with velocity. It fails for two independent reasons.

**Musically**, summing two different recordings of the same drum smears the transient — two
non-coherent attacks arriving together. (The same experiment on piano produced audible warble on
long decays, from the independent beating of each recording's unison strings.)

**Structurally**, it cannot be made to switch cleanly. Linear params — which the oscillator volumes
are — combine *multiplicatively* in the patcher:

```
running_total *= (536870912 + contribution) / 536870912
```

Two consequences:

- a preset value of `0x80000000` scales to exactly `-536870912`, making `running_total` zero — and
  every later cable multiplies that zero. **An oscillator parked at minimum is silent forever and
  cannot be patched back up.**
- a full-scale cable drives the factor through zero mid-sweep, producing a silent notch and then a
  sign flip

Because an oscillator can only be *scaled*, never offset up from silence, a velocity crossfade is
forced to be centred — both layers audible at velocity 64, which is exactly where
non-velocity-sensitive pads sit. There is also an inherent ~6 dB dip at the crossover, since
`getFinalParameterValueVolume` squares the patched value and two layers at quarter amplitude sum to
half.

Safe pair, if you ever do want a crossfade: `±0x40000000` cables against a `0x00000000` preset.
Each oscillator then reaches zero exactly at its far velocity extreme and never crosses it.

## Interim workaround

Until this lands, `PatchSource::RANDOM` is sampled once per note-on (`voice.cpp`, alongside the
velocity setup) and held for that voice's life — a genuine per-hit random. Patched at small depth
to `volume` and `pitch` it breaks up the identical-transient effect that makes repeated hits sound
mechanical. It is not round-robin — same sample every time — but it is free and available today.

`pitch` (`LOCAL_PITCH_ADJUST`) is an exp param and combines additively, so it carries none of the
multiplicative hazard described above. From `getExp`, `2^26` of patched value is exactly one
octave, so one cent is `67108864 / 1200 ≈ 55924`.

Beware the cable amount, though: pitch destinations do **not** use the raw amount.
`getModifiedPatchCableAmount()` (`patch_cable_set.cpp`) squares it and divides by √2 —
`strength = ((A >> 15) * (A >> 16)) * 0.7071` — so the swing is roughly `A²/2³¹ * 0.707`, not
`A/2`. That is a pitch/delay-rate exception; linear params really do read the raw amount
(`combineCablesLinear` in `patcher.cpp`), which is what makes it easy to miss. Solved against the
real arithmetic, useful random→pitch amounts are:

| Swing | Amount |
|---|---|
| ±4.7 cents | `0x02620000` |
| ±9.4 cents | `0x035E8000` |
| ±14 cents | `0x041D0000` |

## Round-robin

Deliberately out of scope here, and a much larger change. `MultisampleRange` holds exactly one
`sampleHolder`, and `MultiRangeArray` is a fixed-`elementSize` array, so alternates cannot live
inline — they need per-range heap storage plus a rotating counter on the Drum (not the Voice;
voices are transient). The cheaper variant is random-pick-excluding-last, which needs one byte of
state per drum.

Velocity layers should land first: the note key is already free, so the cost is far lower for
comparable musical benefit.

## What landed

Phase 1 and Phase 2, gated on the `DrumVelocityLayers` community feature (`drumVelocityLayers` in
`SETTINGS/CommunityFeatures.XML`), default off.

- `Sound::rangesKeyedByVelocity()` is the gate; `SoundDrum` overrides it to return the feature
  state. `Sound::getRangeKey(transposedNote, velocity)` picks which key to search with, and the
  four range-lookup call sites go through it. `Source::getRange()` is unchanged apart from its
  parameter being renamed `key`.
- The two length queries in `sound.cpp` run before any velocity is known, so they ask about the
  loudest layer (`kVelocityForLengthQuery`).
- Serialization writes `rangeTopVelocity` for velocity-keyed sources and reads **either**
  attribute always, so presets keep loading whichever way the feature is set.
- The range editor (`multi_range.cpp`, `sample/transpose.h`) prints plain numbers instead of note
  names for a velocity-keyed source, and the menu is titled "Velocity range". The range-nudging
  and range-splitting arithmetic already worked on plain 0–127 values and needed no change.

Verified in DelugEmu with a one-drum kit whose three ranges are 220 Hz / 1 kHz / 4 kHz tones split
at `rangeTopVelocity` 62 and 63. Those bounds separate the two lookups: a drum always sounds
`kNoteForDrum` (60) and the default audition velocity is 64, so note-keying must pick range 0 and
velocity-keying must pick range 2. Auditioning the row gave 220 Hz with the feature off and 4 kHz
with it on, and re-saving the song wrote `rangeTopVelocity` back with the same bounds.

### Phase 3, the import

A third option, "Velocity layers", sits beside "Load all" and "Slice" in the sample browser's
context menu (long-press SELECT). It imports the folder as the layers of the **one drum being
edited**: the files in filename order, softest first, with the velocity range split evenly and the
last layer left open-ended so it catches 127. The drum takes the folder's name.

Because it only rewrites the ranges of a drum that already exists, it deliberately does **not**
carry the brand-new-kit restriction that "Load all" and "Slice" have — those add drums, this does
not. The menu now opens whenever either kind of import is possible and hides the options that
aren't, so on an established kit "Velocity layers" is the only one offered.

Two things worth knowing about the folder it expects:

- One folder is one drum. It must hold exactly one file per layer — no round-robin alternates and
  no second articulation mixed in, or they become layers of their own.
- Ordering is the browser's existing `strcmpspecial`, which compares digit runs numerically, so an
  unpadded `vl1 … vl36` ladder sorts correctly. It also interprets note names in filenames, which
  is wanted for multisamples and merely harmless here unless a layer name happens to contain one.

### Verified

The three phases were checked in DelugEmu.

Playback and serialization, with a one-drum kit whose three ranges are 220 Hz / 1 kHz / 4 kHz tones
split at `rangeTopVelocity` 62 and 63. Those bounds separate the two lookups: a drum always sounds
`kNoteForDrum` (60) and the default audition velocity is 64, so note-keying must pick range 0 and
velocity-keying range 2. Auditioning gave 220 Hz with the feature off and 4 kHz with it on, and
re-saving the song wrote `rangeTopVelocity` back with the same bounds.

The import, against the real Virtuosity Drums library: `mid_snare_center_vl1 … vl36` converted to
WAV with the names left unpadded. It produced 36 ranges, in true numeric order (`vl2` before
`vl10`), bounds strictly increasing at 3, 7, 10 … 123 with the last open-ended, the drum renamed
after the folder, and the other drum in the kit untouched. The imported drum sounded.

### Still open

The memory cost of N resident layers per drum has **not** been measured properly. The 36-layer
import above (11 MB of samples on one drum) completed and played without running out, which is a
useful data point but not a measurement, and says nothing about nine such drums at once.
