---
name: deluge-preset-xml
description: Author Deluge SYNTH (<sound>) and KIT (<kit>) preset XML with the shared deluge_preset.py module, validate it against the firmware's real patcher/loader rules, verify it loads in DelugEmu, then copy it to the SD card.
---

# Deluge preset XML (SYNTHS/ and KITS/)

Use this skill whenever a task is "make a Deluge synth preset / kit from these samples (or
these synth settings)". Do not hand-write the XML or re-derive the hex encoding: build it
with `deluge_preset.py` in this directory, which mirrors the firmware's own serializer and
patcher arithmetic, and run its `validate()` before anything touches a card.

Files here:

- `deluge_preset.py` - the module (stdlib only; runs anywhere `dbt` runs).
- `test_deluge_preset.py` - `python3 -m unittest test_deluge_preset`.
- `examples/plaits_drums_kit.py` - synth-drum kit from the Plaits drum models (fork only).
- `examples/sample_kit_velocity_ladder.py` - sample kit with velocity layers + humanisation.
- `examples/multisample_piano_crossfade.py` - multisample synth, single layer and a
  two-oscillator velocity crossfade, with the patcher sweep printed.
- `examples/diff_presets.py OLD NEW` - semantic diff of two preset files (either style).

Every example runs with no arguments and writes to `$DELUGE_STAGING` (default
`~/deluge-staging`), creating placeholder WAVs where a real library is not staged.

## Workflow

1. **Gather sources.** Samples must be WAV/AIFF - the Deluge cannot read FLAC (or MP3);
   convert to 44.1 kHz / 16-bit WAV (`ffmpeg -af aresample=...:osf=s16 -ar 44100
   -c:a pcm_s16le`; apply any makeup gain in the same chain, before the 16-bit dither).
   44.1 kHz plays 1:1 with no resampling. Take keymaps / velocity bands from the library's
   own metadata (SFZ `lokey/hikey/pitch_keycenter`, `lovel/hivel`), never from filenames or
   an invented even split. Stage the WAVs under `<staging>/SAMPLES/<Library>/...`.
2. **Build with the module.** `Sound` for a synth, `Kit` + `Kit.drum()` rows for a kit;
   `Osc.set_sample()` / `Osc.set_ranges()` for samples; `set()` / `envelope()` / `cable()`
   for params. Encode values with `half()`, `full()`, `knob()`, `db()`, `semitones()`,
   `cents()`; pick envelope values with `release_seconds()` and friends.
3. **Validate, then write to the staging dir.** `rep = preset.validate(staging_root)`;
   print it; stop on errors. Write `<staging>/SYNTHS/Name.XML` or `<staging>/KITS/Name.XML`.
   Sample paths inside the file are relative to the card ROOT (`SAMPLES/...`) even when the
   preset lives in a subfolder like `KITS/Tim/`.
4. **Verify in DelugEmu** (see memory notes `delugemu-launch`, `delugemu-testing`,
   `delugemu-ui-driving`). The emulator's card is
   `~/Library/Application Support/DelugEmu/sdcard_rw/` with `SYNTHS/`, `KITS/`, `SAMPLES/`.
   - `pkill -f DelugEmu` first and re-check with `pgrep`: a running instance snapshots the
     card at launch and writes it back on exit, clobbering anything copied in meanwhile.
   - Copy the XML and its samples in. Name the test preset so it sorts FIRST in its folder
     (e.g. `KITS/! MYKIT.XML`): pressing KIT (`w`) loads the first kit; for a synth press
     KIT then SYNTH (`q`) to swap to the first synth. Browsing to a specific name over QMP
     is fiddly.
   - Launch: `/Applications/DelugEmu.app/Contents/MacOS/DelugEmu <deluge.bin> --display
     none -- -qmp unix:/tmp/dz_qmp.sock,server,nowait`. The classic boot signal
     `rza1l-ssif: tx-render-head auto-detected` in
     `~/Library/Application Support/DelugEmu/delugemu.log` did NOT appear in the 2026-08-27
     runs (coreaudio backend, no `--audio wav`), and the launcher's own "SSIF audio bounded
     by auto-detected firmware render head" line is a false match for it. Wait for
     "Launching deluge machine" in the log, then ~75 s (the 700 MB card image prep can take
     a minute before that), and confirm readiness with a screendump. The boot song opens
     the FIRST preset in `SYNTHS/`, so a sorted-first synth is already loaded at boot.
   - Over QMP send `input-send-event` key `w` (and `q`), wait 4 s, `human-monitor-command`
     `screendump x.ppm`, `sips -s format png`, and LOOK at the OLED: it must show the
     preset name, with no `FILE_CORRUPTED` / `SD card error` popup. Audition pads are
     sidebar column 17 (`x = 2073 + 119`, `y = 632 + screenRow*119`) if you want audio.
   - Round-trip check (optional, the strongest one): press SAVE (`s`) then SELECT (`ret`).
     In clip view that opens SAVE SONG, not save-preset, and writes `SONGS/SONGn.XML`
     embedding the loaded instrument: the `<sound>` block under `<instruments>` carries
     the structure (oscs, ranges, lfos, modKnobs, delay, sidechain) and the clip's
     `<soundParams>` carries defaultParams / envelopes / patchCables. `quit` over QMP so the
     card is written back, splice the two blocks into one `<sound>` and diff with
     `deluge_preset.flatten_preset_xml()`. Expected differences only: `endSamplePos` 0 ->
     the sample length (the loader resolves it), the preset name, and firmware defaults the
     module omits (`audioCompressor`, `stutter`, `midiOutput`, song attributes). Any other
     DIFF is a bug. Verified this way on 2026-08-27 for the crossfade example.
   - Fork-only features need their community toggle ON in
     `SETTINGS/CommunityFeatures.XML` on the emulator card (the root-level copy is ignored).
5. **Copy to the SD card.** Stop all emulators and detach stale `DELUGE` disk images first
   (`diskutil list`), confirm `/Volumes/DELUGE` is the real card (`deluge.bin` at the root,
   no root `CommunityFeatures.XML`), then copy `SYNTHS/`, `KITS/` and `SAMPLES/` over.
   When a row list changes, remove the old `SAMPLES/<Library>` first (row numbers are
   baked into filenames) after checking `grep -rl SAMPLES/<Library> /Volumes/DELUGE/SONGS`.

## Gotchas the module encodes (all verified in firmware source)

**Linear params multiply, they do not add.** `Patcher::cableToLinearParam`
(`modulation/patch/patcher.cpp`) does `running_total *= (536870912 + contribution) /
536870912`, and the preset value is folded in as the first "cable". Consequences:

- A preset of `0x80000000` scales to exactly zero, so an oscillator parked at minimum is
  **silent forever and cannot be patched back up**. `validate()` makes this an error.
- A full-scale `0x7FFFFFFF` cable drives the factor through zero mid-sweep: a silent notch
  (around velocity 24 for velocity->oscAVolume) and then a sign flip. `validate()` warns.
- The safe velocity crossfade is `+/-0x40000000` cables against `0x00000000` presets: each
  osc reaches zero exactly at its far velocity extreme.
- `getFinalParameterValueVolume` (`util/functions.cpp`) then **squares** the result, so a
  centred crossfade has an inherent ~6 dB dip at velocity 64 (each layer at 1/4 amplitude).
  A centred preset (`0x00000000`) is -12 dB on its own; `db()` accounts for the square law.
- Linear cable calibration from a full-scale bipolar source: `0x04000000` = +/-1.1 dB,
  `0x08000000` = +/-2.2 dB, `0x10000000` ~ +/-4 dB. `Patcher.gain_db()` computes any case.

**Envelope attack/decay/release values are table indices, not rates.** `combineCablesExp`
scales the preset by `paramRanges[p]` (1073741824 for decay/release, 805306368 for
attack; `patcher.cpp`), the result indexes `releaseRateTable64` through
`lookupReleaseRate` (`util/functions.cpp`), and `Envelope::render` adds that rate per sample
until 8388608. Knob 6 = 0.36 s, 20 = 1.64 s, 30 = 3.46 s, 50 = 27 s of release; decay is
twice that. Use `knob()` for the value and `release_seconds()` / `decay_seconds()` /
`attack_seconds()` / `knob_seconds_table()` to choose it.

**The `velocity` patch source is bipolar about 64**: `(velocity - 64) * 33554432`
(`model/voice/voice.cpp`; 128 = INT32_MAX). Velocity 64 is a cable's zero, which is why the
crossfade must be centred there and why `note` (same formula) behaves the same way.

**Pitch destinations square the cable amount.** `PatchCableSet::getModifiedPatchCableAmount`
squares amounts to `pitch`, `oscAPitch`, `oscBPitch`, `modulator*Pitch` and `delayRate`,
then scales master `pitch` by 2/3 (from velocity) or 1/sqrt2 (anything else). One octave
is 2^26 of patched value, one cent 55924. `0x035E8000` random->pitch = +/-9.4 cents;
amounts chosen by analogy with volume are ~1000x too weak. `pitch_cable_amount(semis)`
solves it; `semitones()` / `cents()` are for the *preset* space (2^29 per octave).

**sampleRange rules** (`Sound::readSourceFromFile`, `processing/sound/sound.cpp`):

- The **last** range must omit its bound (`rangeTopNote` / `rangeTopVelocity`); it defaults
  to 32767 and catches the top of the keyboard or velocity 127.
- Duplicate bounds are rejected as `FILE_CORRUPTED`. `sample_ranges()` enforces both.
- A one-sample osc writes NO `<sampleRanges>` wrapper: `fileName` and `<zone>` go straight
  on `<osc1>`. `Osc.set_sample()` does this.
- Stock firmware has **no velocity layers** - `MultiRange` is keyed on `topNote` only. On
  `local-fixes`, kit drums are keyed by velocity when the `DrumVelocityLayers` community
  feature is ON (a drum always sounds note 60, so the note key was unused); the file then
  says `rangeTopVelocity`. Use `Osc.set_ranges(..., keyed="velocity")`.
- `transpose = 60 - rootMidiNote` (`sample_holder_for_voice.cpp`). `transpose` and `cents`
  are read back verbatim and never re-derived, so always write them for pitched
  multisamples; omit them (0) for drums, which play at their natural rate.
- `endSamplePos="0"` means "to the end of the sample" (`sample_holder.cpp`).
- `loopMode` 0 = CUT (note-off damps, right for piano), 1 = ONCE (drum one-shots,
  note-off ignored), 2 = LOOP, 3 = STRETCH (`definitions_cxx.hpp`).

**Root notes from WAV `smpl` chunks.** The loader reads `dwMIDIUnityNote` from the `smpl`
chunk (`audio_file.cpp`); its chunk scanner runs to EOF, so a `smpl` appended after `data`
is still found (`stamp_root_note()` does that). If **every** file in a folder carries the
**same** unity note the loader discards them all as suspicious and pitch-detects instead.
Explicit `transpose` in the preset sidesteps all of this.

**No FLAC.** Convert to 44.1 kHz / 16-bit WAV. `validate()` rejects non-WAV/AIFF names and
warns about other rates/bit depths.

**Enumerations.** `filterRoute` is `H2L` / `L2H` / `PARA`; `lpfMode`/`hpfMode` are `12dB`,
`24dB`, `24dBDrive`, `SVF_Band`, `SVF_Notch`, `HPLadder`, `Off`; `polyphonic` is `auto`,
`poly`, `mono`, `legato`, `choke` (one choke group per kit - every choke drum chokes all
the others; `"0"` in old files means auto). Patch sources: `lfo1..4`, `envelope1..4`,
`velocity`, `note`, `compressor`, `random`, `aftertouch`, `x`, `y`. `random` is latched
once per note-on, so a random->volume/pitch cable is per-hit humanisation.

**Unipolar vs bipolar params.** `oscAPulseWidth` / `oscBPulseWidth` are unipolar
`0x00000000..0x7FFFFFFF` (`half()`); every other param is bipolar
`0x80000000..0x7FFFFFFF` (`full()` / `knob()`). `validate()` checks ranges. Note the trap
that the `<defaultParams volume=...>` attribute is `GLOBAL_VOLUME_POST_FX` while the cable
destination `volume` is `LOCAL_VOLUME`, whose preset is fixed at 0 and never written.

**Kit facts.** Rows appear on the grid bottom-up in file order. Sample-drum defaults
(`Sound::setupAsSample`): env1 A0 D20 S50 R0, one `velocity -> volume` cable at
`0x3FFFFFE8` (`Kit.drum()` applies these). A kick usually sends to the sidechain
(`sideChainSend="2147483647"`). Layered rows should drop velocity->volume to ~`0x10000000`
because the samples already carry 20-30 dB of dynamics. `Kit.validate()` measures each
staged WAV and fails the build if a velocity ladder does not ascend - a louder sample in a
lower band is inaudible as a bug and very audible as a drum that gets quieter when hit
harder (this shipped once; sort layers by measured level before assigning bands).

## Fork-only attributes (presets from `local-fixes` features on stock firmware)

| Attribute / feature | Community toggle | On stock firmware |
|---|---|---|
| `<osc1 type="drum" drumModel="808kick\|808snare\|hihat\|909kick\|909snare\|hihat2">` (Plaits drum models, #31) | Drum Models | `stringToOscType("drum")` falls back to TRIANGLE: loads, plays a triangle wave. Silent fallback. |
| `rangeTopVelocity` on kit-drum `<sampleRange>` (velocity layers) | Drum Velocity Layers | **Not silent.** Unknown name -> every range keeps topNote 32767 -> duplicate-bound check -> `FILE_CORRUPTED`, shown as "SD card error" (the loader's `if (!fileSuccess)` inverts the FRESULT). On the fork with the toggle OFF the row plays only the layer covering note 60. |
| `tapeSaturation` in `<defaultParams>` | Tape saturation FX | Unknown attribute, skipped. Silent. |
| `CHORDS/*.XML` chord sets | Chord library sets | Separate files; ignored. |
| Accent tiers (#29) | (planned) | Will add a kit-level attribute - update `Kit` when it lands. |

`validate()` prints a note for each fork-only feature a preset uses.

## Serializer facts the module follows

- Root: `<sound firmwareVersion="c1.3.0" earliestCompatibleFirmware="4.1.0-alpha" ...>`
  (`Serializer::writeFirmwareVersion` writes `kFirmwareVersionStringShort` = `c` +
  `PROJECT_VERSION`; `Output::writeToFile` adds the earliest-compatible string). The
  device writes attribute-style XML; the reader accepts the old `<tag>value</tag>` style too.
- `<sound>` element order (`Sound::writeToFile`): attributes `name` (drum rows only),
  `polyphonic`, `voicePriority`, `sideChainSend` (if non-zero), `mode`, `transpose` (if
  non-zero), `modFXType`, `lpfMode`, `hpfMode`, `filterRoute`, `clippingAmount`, `path`
  (drum rows), `maxVoices`; then `<osc1>`, `<osc2>`, `<lfo1..4>`, `<modulator1/2>` (FM
  only), `<unison>`, `<defaultParams ...>` with `<envelope1..4>`, `<patchCables>`,
  `<equalizer>`, then `<arpeggiator>`, `<modKnobs>`, `<midiOutput>`, `<delay>`,
  `<sidechain>`, `<audioCompressor>`, `<stutter>`. The blocks the module leaves out are
  optional to the reader and take firmware defaults.
- `<kit>` (`Kit::writeToFile`): attributes `modFXCurrentParam`, `currentFilterType`,
  `modFXType`, `lpfMode`, `hpfMode`, `filterRoute`; then `<defaultParams>` with `<delay>`,
  `<lpf>`, `<hpf>`, `<equalizer>`, then `<delay>`, `<sidechain>`, `<soundSources>` of
  `<sound name=...>` rows, `<selectedDrumIndex>`.
- `knob(50)` is `0x7FFFFFD2`, exactly what the device writes for a knob at 50 - older
  generators wrote `0x7FFFFFFF`, which is equivalent but drifts if re-saved.

## Quick reference

```python
import sys

sys.path.insert(0, ".claude/skills/deluge-preset-xml")
import deluge_preset as dp

# --- a kit row with velocity layers and humanisation
kit = dp.Kit()
snare = kit.drum("SNARE")  # auto poly, env A0 D20 S50 R0, vel->volume 0x3FFFFFE8
snare.osc1.set_ranges(
    [
        ("SAMPLES/X/snare v1.wav", 40),
        ("SAMPLES/X/snare v2.wav", 90),
        ("SAMPLES/X/snare v3.wav", None),
    ],
    keyed="velocity",
    loop_mode=dp.LOOP_MODE_ONCE,
)
snare.cables = [
    dp.PatchCable("velocity", "volume", "0x10000000"),
    dp.PatchCable("random", "volume", "0x04000000"),
    dp.PatchCable("random", "pitch", dp.pitch_cable_amount(0.094)),
]
kit.drum("HH", polyphonic="choke").osc1.set_sample(
    "SAMPLES/X/hh.wav", loop_mode=dp.LOOP_MODE_ONCE
)
print(kit.validate(staging_root))
kit.write(f"{staging_root}/KITS/My Kit.XML")

# --- a multisample synth
s = dp.Sound(name="Piano", max_voices=16)
s.osc1.set_ranges(
    [
        dp.SampleRange("SAMPLES/P/C3.wav", top=53, transpose=60 - 48),
        dp.SampleRange("SAMPLES/P/C4.wav", top=None, transpose=60 - 60),
    ],
    loop_mode=dp.LOOP_MODE_CUT,
)
s.set(volume=dp.knob(30), reverbAmount=dp.knob(12)).envelope(1, release=20)  # 1.64 s
s.cable("velocity", "volume", "0x3FFFFFE8").cable(
    "velocity", "lpfFrequency", "0x0F5C28F0"
)
print(s.validate(staging_root))
s.write(f"{staging_root}/SYNTHS/Piano.XML")

# --- check a cable design before trusting it
dp.Patcher.gain_db("oscAVolume", "0x00000000", s.cables, {"velocity": 64})
dp.Patcher.silent_velocities("oscAVolume", preset_hex, cables)
```
