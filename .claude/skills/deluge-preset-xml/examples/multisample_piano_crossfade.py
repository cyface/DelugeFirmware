#!/usr/bin/env python3
"""Example (c): a multisample synth with a two-layer velocity crossfade.

Trimmed from ~/WebstormProjects/SalamanderGrandPiano/Deluge/make_deluge_piano.py. The real
script converts the FLAC library (ffmpeg -> 44.1k/16-bit WAV, +4 dB on the soft layer),
stamps smpl root notes and takes the keymap from the library's SFZ; this example shows the
preset side and writes placeholder WAVs when the library is not staged.

Two presets are written under the staging dir:
  SYNTHS/Example Piano.XML     one layer on OSC1, velocity -> volume + LPF (stock, safe)
  SYNTHS/Example Piano VX.XML  soft layer on OSC1 fades out as the hard layer on OSC2
                               fades in - the crossfade design, with its caveats

The crossfade carries the two hardest-won patcher facts (patcher.cpp cableToLinearParam):
  * linear params MULTIPLY: running_total *= (536870912 + contribution) / 536870912.
    A preset of 0x80000000 scales to zero, so an osc parked at minimum is silent forever
    and cannot be patched back up. A full-scale 0x7FFFFFFF cable drives the factor through
    zero mid-sweep (silent notch, then sign flip).
  * the safe pair is +/-0x40000000 cables against 0x00000000 presets: each osc reaches
    zero exactly at its far velocity extreme. getFinalParameterValueVolume then SQUARES
    the value, so the centred crossfade has an inherent ~6 dB dip at velocity 64 (two
    layers at 1/4 amplitude sum to 1/2). validate() reports the sweep.
"""

import os

from _common import dp, report, staging_root

SOFT_DIR = "SAMPLES/Examples/Piano/Soft"
HARD_DIR = "SAMPLES/Examples/Piano/Hard"

# (hikey, root) per region, from the library's SFZ (lokey/hikey/pitch_keycenter). The
# last region's hikey is not used: the last range is left open so it catches the top.
REGIONS = [
    (38, 36),
    (45, 43),
    (52, 50),
    (59, 57),
    (66, 64),
    (73, 71),
    (80, 78),
    (87, 85),
]

RELEASE = 20  # knob 20 -> 1.64 s note-off tail (dp.release_seconds(20))
REVERB = 12  # knob 12 -> a small room
VOLUME = 30  # knob 30 leaves headroom for chords (the cable pushes above the preset)
MAX_VOICES = 16
XFADE = 0x40000000  # the safe crossfade amount


def note_name(midi):
    return (
        "CDEFGAB"[[0, 2, 4, 5, 7, 9, 11].index(midi % 12)] + str(midi // 12 - 1)
        if midi % 12 in (0, 2, 4, 5, 7, 9, 11)
        else str(midi)
    )


def ranges_for(sd_dir):
    out = []
    for i, (hikey, root) in enumerate(REGIONS):
        out.append(
            dp.SampleRange(
                f"{sd_dir}/{note_name(root)}.wav",
                top=None if i == len(REGIONS) - 1 else hikey,
                transpose=60 - root,
            )
        )  # sample_holder_for_voice.cpp
    return out


def ensure_placeholders(root):
    made = 0
    for sd_dir, level in ((SOFT_DIR, -12.0), (HARD_DIR, -4.0)):
        for _, midi in REGIONS:
            p = os.path.join(root, sd_dir, note_name(midi) + ".wav")
            if not os.path.isfile(p):
                # stamp the root note so the on-device folder import also works; the
                # notes differ per file, so the "all the same unity note" trap is avoided
                dp.write_tone_wav(
                    p,
                    peak_dbfs=level,
                    seconds=0.1,
                    freq=440.0 * 2 ** ((midi - 69) / 12.0),
                    unity_note=midi,
                )
                made += 1
    if made:
        print(
            f"wrote {made} placeholder WAVs under {os.path.join(root, 'SAMPLES/Examples/Piano')}"
        )


def base_preset(name):
    s = dp.Sound(
        name=name,
        polyphonic="poly",
        mode="subtractive",
        lpf_mode="24dB",
        hpf_mode="HPLadder",
        filter_route="H2L",
        max_voices=MAX_VOICES,
    )
    s.set(
        volume=dp.knob(VOLUME),
        reverbAmount=dp.knob(REVERB),
        delayFeedback="0x80000000",
        lpfResonance="0x80000000",
    )
    s.envelope(1, attack=0, decay=20, sustain=50, release=RELEASE)
    s.delay = {"pingPong": 1, "analog": 0, "syncLevel": 7, "syncType": 0}
    return s


def single_layer(name, sd_dir):
    s = base_preset(name)
    s.osc1.set_ranges(ranges_for(sd_dir), keyed="note", loop_mode=dp.LOOP_MODE_CUT)
    s.osc2 = dp.Osc("square")
    s.set(oscAVolume="0x7FFFFFFF", oscBVolume="0x80000000")
    s.cable("velocity", "volume", "0x3FFFFFE8")
    s.cable("velocity", "lpfFrequency", "0x0F5C28F0")
    return s


def crossfade(name):
    s = base_preset(name)
    s.osc1.set_ranges(ranges_for(SOFT_DIR), keyed="note", loop_mode=dp.LOOP_MODE_CUT)
    s.osc2.set_ranges(ranges_for(HARD_DIR), keyed="note", loop_mode=dp.LOOP_MODE_CUT)
    s.set(oscAVolume="0x00000000", oscBVolume="0x00000000")  # centred, NOT 0x80000000
    s.cable("velocity", "oscAVolume", (-XFADE) & 0xFFFFFFFF)  # soft fades out
    s.cable("velocity", "oscBVolume", XFADE)  # hard fades in
    s.cable("velocity", "lpfFrequency", "0x0F5C28F0")
    return s


def print_crossfade_table(s):
    print("velocity crossfade as the patcher computes it (dB re full scale):")
    print("  vel   oscA    oscB")
    for v in (1, 16, 32, 48, 64, 80, 96, 112, 127):
        a = dp.Patcher.gain_db(
            "oscAVolume", s.preset_for("oscAVolume"), s.cables, {"velocity": v}
        )
        b = dp.Patcher.gain_db(
            "oscBVolume", s.preset_for("oscBVolume"), s.cables, {"velocity": v}
        )
        print(f"  {v:3d} {a:7.1f} {b:7.1f}")


def main():
    root = staging_root()
    ensure_placeholders(root)
    for s in (single_layer("Example Piano", SOFT_DIR), crossfade("Example Piano VX")):
        path = os.path.join(root, "SYNTHS", s.name + ".XML")
        rep = s.validate(root)
        s.write(path)
        report(rep, path)
    print_crossfade_table(crossfade("Example Piano VX"))
    print(f"release knob {RELEASE} = {dp.release_seconds(RELEASE):.2f} s")


if __name__ == "__main__":
    main()
