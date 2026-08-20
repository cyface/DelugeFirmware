# Authoring KIT and SONG files outside the Deluge

Notes for generating `KITS/*.XML` and `SONGS/*.XML` from a script rather than from the device —
sample-library importers, MIDI-to-song converters, batch preset builders.

The formats are readable but not forgiving, and several rules produce a file that *loads without
error* and then behaves wrongly: notes silently dropped, a drum that gets quieter when you hit it
harder, a clip that will not open or play. Each of those cost real time to find, so they are
written down here with the source that governs them.

Line references are against the tree this document was added to; re-check them after a rebase.

## The two shapes of a kit

A kit exists in two different serializations and they are **not interchangeable**.

**Standalone `KITS/*.XML`** — root `<kit>`, holding a `<soundSources>` array of `<sound>`, one per
drum row. Each `<sound>` carries its own `<defaultParams>` (envelopes, patch cables) and
`<equalizer>`. Rows appear on the grid bottom-up in file order.

**A kit embedded in a song** — the same `<kit>` under the song's `<instruments>`, but the per-drum
parameters are **not** in the `<sound>`. They live per row, in the clip's
`<noteRow>/<soundParams>`. A song whose `<sound>` blocks still carry `<defaultParams>` and whose
note rows carry no `<soundParams>` loads fine and quietly loses every patch cable — for a
velocity-layered kit that means losing velocity → volume and all humanisation.

So converting a standalone kit into a song means *moving* each `<defaultParams>` into that row's
`<soundParams>`, with `<equalizer>` moved inside it.

## Notes: the hex blob

Notes are an attribute on `<noteRow>`, not child elements
(`note_row.cpp:3575` writer, `:3518` reader). `noteDataWithSplitProb` is the current form, 28 hex
characters per note, big-endian, concatenated with no separator:

| field | hex chars | notes |
|---|---|---|
| `pos` | 8 | int32, in Deluge ticks |
| `length` | 8 | int32 |
| `velocity` | 2 | 1–127; 0 or >127 is coerced to 64 |
| `lift` | 2 | `kDefaultLiftValue` = 64 (`definitions_cxx.hpp:716`) |
| `probability` | 2 | `kNumProbabilityValues` = 20 means 100% (`:708`) |
| `iterance` | 4 | `kDefaultIteranceValue` = 0 means off (`:712`) |
| `fill` | 2 | `FillMode::OFF` = 0 |

Older forms still read: `noteDataWithLift` (22 chars) and `noteDataWithIteranceAndFill` (26), both
gated on the song's firmware version. Write the 28-char form.

**Overlapping notes are silently dropped.** The reader tracks `minPos` as the previous note's
`pos + length` and skips anything starting before it (`note_row.cpp:3470`):

```cpp
if (pos < minPos || pos > kMaxSequenceLength - length) {
    continue;
}
```

Notes must therefore be sorted ascending within a row and each note's length clamped to the gap
to the next one. Drum samples are one-shots (`loopMode="1"` = ONCE) so length is not audible, but
get it wrong and hits vanish with no diagnostic.

**The ladder must ascend.** Nothing in the format enforces that a velocity split's ranges rise in
level, and nothing warns if they do not. Any generator that picks layers by "nearest sample to a
target level" can emit a non-monotonic ladder when the source material is sparse — the nearest
unused candidate for a low target can be louder than the one already taken for a higher target.
The result is a drum that gets quieter when you hit it harder, across one velocity boundary.
Sort chosen samples by measured level *after* selection and before assigning `rangeTopVelocity`,
and assert the ladder ascends.

Note also that a one-sample oscillator writes **no** `<sampleRanges>` wrapper — `fileName` and
`<zone>` go straight on `<osc1>`. The last range omits `rangeTopVelocity`, which defaults it to
32767 so it catches velocity 127. Duplicate bounds are rejected as `FILE_CORRUPTED`.

Velocity-keyed ranges require the `drumVelocityLayers` community feature; see
[drum_velocity_layers.md](drum_velocity_layers.md). With it off, every row plays only the layer
covering `kNoteForDrum` = 60 (`definitions_cxx.hpp:1006`).

## Time and tempo

Tick resolution is per song: `getBarLength() = 96 << inputTickMagnitude` (`song.cpp:5726`). At the
common magnitude 2 a bar is 384 ticks, a quarter note 96, a sixteenth 24.

That makes conversion from a MIDI file an exact integer divide when the two grids line up — a
960 ppq file divides by 10 into magnitude 2. **Check that every event lands on the grid and refuse
if not**, rather than rounding a performance onto a grid without saying so.

Tempo is stored as a 64-bit fixed-point number split across two attributes
(`song.cpp:3126 setBPMInner`):

```
timePerTimerTick = 110250 / (BPM << inputTickMagnitude)      // magnitude > 0
timePerTimerTickBig = timePerTimerTick * 2^32 + 0.5
timePerTimerTick  attribute = big >> 32
timerTickFraction attribute = (uint32)big, written SIGNED
```

Worked check: 140 BPM at magnitude 2 gives 196 / `0xE0000000`, which appears in the file as
`timePerTimerTick="196" timerTickFraction="-536870912"`.

## A song must name a current clip

This one is worth its own heading because it presents as "the file is broken".

If no clip is marked as current, the song loads with **no current clip at all**. CLIP_VIEW then has
nothing to open, and PLAY produces silence. The clip is reachable only by leaving the song and
loading it again. Nothing is logged.

The pointer is `selected="1"` on the clip (`clip.cpp:739`):

```cpp
else if (!strcmp(tagName, "selected")) {
    if (reader.readTagOrAttributeValueInt()) {
        song->setCurrentClip(this);
        song->inClipMinderViewOnLoad = false;
    }
}
```

Its sibling `beingEdited="1"` (`clip.cpp:678`) does the same and additionally opens the song
straight into clip view rather than session view. The device writes whichever matches the state it
was in when saved, so a generated song needs one of them explicitly.

## Display attributes that are not cosmetic

- **`colourOffset="0"` on a clip renders its 16 pads black.** The row then looks like an empty slot
  in session view, and pressing it *creates a new clip* instead of launching the existing one. Use
  a non-zero offset.
- **Session view draws clip index 0 at the bottom** and counts upward, so with no scroll a single
  clip sits on the bottom row. The row displayed at `y` shows clip index `y + yScrollSongView`.

  The value the device writes is exactly **`numClips - 8`** — surveyed across 60+ device-saved
  songs it holds every time (1 clip → −7, 2 → −6, 3 → −5, … 50 → 42), the only exceptions being
  songs deliberately scrolled elsewhere. In other words the Deluge always scrolls so clips fill
  the 8-row display from the *top*. Compute the same thing; a generated song that omits it looks
  wrong in a way that is easy to mistake for a bug elsewhere.
- `preview` / `previewNumPads` are the browser thumbnail only. Safe to omit.

## Verifying a generated file

Two techniques worth more than reading the XML back yourself.

**Let the firmware serialize it.** Load the file on the device or in DelugEmu, save it under a new
name, and diff. The output is exactly what the firmware parsed, so any attribute *it* writes that
yours lacks is a bug in your generator, and anything of yours it dropped was not understood. This
is how `selected` was found: it was the only functional difference between a hand-generated song
and the same song after the device re-saved it.

**Check a control is actually a control.** "A known-good file behaves the same way, so it must be
my tooling" is only valid if the known-good file is intact. A reference song used this way was
silent because all 17 of its kit samples were missing from that SD card — an unrelated cause that
made a real bug look like a tooling problem and delayed finding it. Verify every sample path in a
control resolves before drawing conclusions from it.

## Checklist

- [ ] Kit params in the right place for the shape you are writing (`<defaultParams>` standalone,
      `<soundParams>` per note row in a song)
- [ ] Notes sorted ascending per row, lengths clamped so none overlaps the next
- [ ] Velocity ladders ascend in level; last range has no `rangeTopVelocity`
- [ ] Every event on the tick grid; no silent quantisation
- [ ] Tempo pair computed, fraction written signed
- [ ] Exactly one clip carries `selected="1"` (or `beingEdited="1"`)
- [ ] Clip `colourOffset` non-zero
- [ ] Every `fileName` resolves relative to the card root
- [ ] Check the source material for leading silence before blaming the converter
