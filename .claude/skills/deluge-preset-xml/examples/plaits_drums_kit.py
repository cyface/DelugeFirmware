#!/usr/bin/env python3
"""Example (a): a synth-drum kit from the Plaits drum models (OscType::DRUM, FORK ONLY).

This is contrib/plaits_drums/make_kits.py trimmed down to use deluge_preset. The row
tables are the same; only the XML plumbing is gone. Run examples/diff_presets.py against
the contrib output to see that the two agree parameter for parameter.

Row parameters are the three Plaits macros in 0..1:
  tone  -> oscAPulseWidth        (Plaits TIMBRE, unipolar param -> half())
  decay -> oscAWavetablePosition (Plaits MORPH,  bipolar param  -> full())
  snap  -> carrier1Feedback      (Plaits HARMONICS: drive / snappy / noise -> full())
and `transpose` in semitones from the kit's note (C3); the kick models already play two
octaves down and the snares one octave down.

Writes KITS/808 Models.XML and KITS/909 Models.XML under the staging dir. No samples are
needed. Stock firmware loads these but plays each row as a triangle wave (see FORK_ONLY).
"""

import os

from _common import dp, report, staging_root

# make_kits.py's 16 gold-knob assignments differ from the device default in slot 13
# (pitch instead of portamento) - kept so the kits stay equivalent.
MOD_KNOBS = [(p, s) for p, s in dp.DEFAULT_MOD_KNOBS]
MOD_KNOBS[12] = ("pitch", None)


def row(
    name,
    model,
    tone,
    decay,
    snap,
    transpose=0,
    volume="0x50000000",
    pan=0.0,
    choke=False,
    env_decay=20,
    env_sustain=50,
):
    """env_decay / env_sustain are Envelope 1 knob values (0..50). Drum rows ignore
    note-offs, so a partial sustain shapes the hit as fast-drop-then-tail."""
    s = dp.Sound(
        name=name,
        polyphonic="choke" if choke else "auto",
        voice_priority=1,
        side_chain_send=2147483647,
        mode="subtractive",
        lpf_mode="24dB",
        mod_fx_type="none",
    )
    s.osc1 = dp.Osc("drum", transpose=transpose, drum_model=model)
    s.osc2 = dp.Osc("square")
    s.lfo(1, "sine").lfo(2, "sine")
    s.set(
        oscAPulseWidth=dp.half(tone),
        oscAWavetablePosition=dp.full(decay),
        carrier1Feedback=dp.full(snap),
        volume=volume,
        pan=dp.hex32(round(pan * 0x7FFFFFFF)),
        lpfResonance="0x00000000",
    )
    s.envelope(1, attack=0, decay=env_decay, sustain=env_sustain, release=0)
    s.cable("velocity", "volume", "0x20000000")
    s.mod_knobs = MOD_KNOBS
    return s


# Rows are listed bottom-up on the grid, so the kick goes first. Tuning: kicks play at
# note/4 (65 Hz at C3), snares at note/2 - the transposes pull each row into the classic
# register. Hi-hats were fitted against real 808 hats: Tone ~0.95 puts the band-pass near
# 8 kHz and transpose +8 puts f0 ~414 Hz; lower Tone sounds like a bell.
KIT_808 = [
    row("KICK", "808kick", tone=0.35, decay=0.55, snap=0.30, transpose=-3),
    row("KICK LONG", "808kick", tone=0.25, decay=0.85, snap=0.15, transpose=-5),
    row("SNARE", "808snare", tone=0.3, decay=0.3, snap=0.5, transpose=5),
    row("SNARE SNAP", "808snare", tone=0.15, decay=0.3, snap=0.65, transpose=5),
    row("TOM LO", "808snare", tone=0.20, decay=0.60, snap=0.00, transpose=-7),
    row("TOM MID", "808snare", tone=0.25, decay=0.55, snap=0.00, transpose=-1),
    row("TOM HI", "808snare", tone=0.30, decay=0.50, snap=0.00, transpose=4),
    row(
        "HAT CLOSED", "hihat", tone=0.95, decay=0.15, snap=0.15, transpose=8, choke=True
    ),
    row("HAT OPEN", "hihat", tone=0.95, decay=0.65, snap=0.3, transpose=8, choke=True),
    row(
        "CYMBAL",
        "hihat2",
        tone=0.9,
        decay=1.0,
        snap=0.7,
        transpose=-5,
        env_decay=5,
        env_sustain=6,
    ),
]

KIT_909 = [
    row("KICK", "909kick", tone=0.45, decay=0.70, snap=0.45, transpose=-4),
    row("KICK PUNCH", "909kick", tone=0.60, decay=0.55, snap=0.80, transpose=-2),
    row("SNARE", "909snare", tone=0.40, decay=0.45, snap=0.55, transpose=5),
    row("SNARE SNAP", "909snare", tone=0.55, decay=0.35, snap=0.85, transpose=7),
    row("TOM LO", "909snare", tone=0.30, decay=0.75, snap=0.05, transpose=-7),
    row("TOM MID", "909snare", tone=0.30, decay=0.70, snap=0.05, transpose=-1),
    row("TOM HI", "909snare", tone=0.30, decay=0.65, snap=0.05, transpose=4),
    row(
        "HAT CLOSED", "hihat", tone=0.95, decay=0.15, snap=0.45, transpose=8, choke=True
    ),
    row("HAT OPEN", "hihat", tone=0.95, decay=0.6, snap=0.45, transpose=8, choke=True),
    row("RIDE", "hihat2", tone=0.65, decay=0.9, snap=0.3, transpose=16),
]


def build(rows):
    kit = dp.Kit(mod_fx_type="none")
    for r in rows:
        kit.add(r)
    return kit


def main():
    root = staging_root()
    for name, rows in (("808 Models", KIT_808), ("909 Models", KIT_909)):
        kit = build(rows)
        path = os.path.join(root, "KITS", name + ".XML")
        rep = kit.validate(root)
        kit.write(path)
        report(rep, path)


if __name__ == "__main__":
    main()
