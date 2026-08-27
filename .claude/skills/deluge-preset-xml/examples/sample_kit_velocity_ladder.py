#!/usr/bin/env python3
"""Example (b): a sample kit with velocity layers (a "velocity ladder") per row.

Trimmed from ~/WebstormProjects/virtuosity_drums/Deluge/make_deluge_kit.py and
~/WebstormProjects/crocell_drums/Deluge/make_deluge_kit.py. Those scripts also convert the
source libraries (ffmpeg FLAC -> 44.1k/16-bit WAV, DrumGizmo mix matrix); this example only
shows the Deluge side. When the real samples are not present it writes tiny placeholder
WAVs (sine bursts at ascending peak levels) so validate() can exercise the real checks:
files present, 44.1 kHz / 16-bit, and the ladder ascending.

FORK ONLY: velocity layers use rangeTopVelocity and need the Drum Velocity Layers
community feature ON. Stock firmware rejects the file (FILE_CORRUPTED -> "SD card error");
the fork with the feature OFF plays only the layer covering note 60. Single-sample rows
are stock-compatible.

Design rules carried over:
  * rows are grid order bottom-up; the loudest layer omits its bound so it catches 127;
  * layered rows drop velocity->volume to 0x10000000 (~+/-4 dB) because the samples
    already carry 20-30 dB of dynamics; single-sample rows keep the stock 0x3FFFFFE8;
  * per-hit humanisation: random->volume (linear, multiplicative - stay well under the
    0x40000000 cliff) and random->pitch (exp, squared amount - use pitch_cable_amount());
  * hi-hats share the kit's single choke group; the kick sends to the sidechain.
"""

import os

from _common import dp, report, staging_root

KIT_NAME = "Example Ladder Kit"
SD_SAMPLE_DIR = "SAMPLES/Examples/LadderKit"

# (row name, group, [(velocity band top or None for the loudest, source-ish label)])
# In the real generators the bands come from the library's own SFZ lovel/hivel mapping
# (Virtuosity) or from measured peak levels through the mix matrix (Crocell). Never invent
# an even split when the library tells you its bands.
ROWS = [
    ("KICK", "kick", [31, 63, 95, None]),
    ("SNARE", "drum", [23, 47, 71, 95, 111, None]),
    ("XSTICK", "drum", [None]),
    ("HH CLSD", "metal", [42, 84, None]),
    ("HH OPEN", "metal", [42, 84, None]),
    ("HH PEDAL", "metal", [None]),
    ("TOM LO", "drum", [63, None]),
    ("TOM HI", "drum", [63, None]),
    ("RIDE", "metal", [63, None]),
    ("CRASH", "metal", [None]),
]

# Per-hit humanisation per group: (random -> volume, random -> pitch).
#   volume 0x04000000 = +/-1.1 dB (factor bottoms at 0.938), 0x08000000 = +/-2.2 dB
#   pitch: +/-4.7, 9.4, 14 cents - solved through the squared cable law
HUMANISE = {
    "kick": ("0x04000000", dp.pitch_cable_amount(0.047)),
    "drum": ("0x04000000", dp.pitch_cable_amount(0.094)),
    "metal": ("0x08000000", dp.pitch_cable_amount(0.14)),
}
VELOCITY_TO_VOLUME_LAYERED = "0x10000000"
VELOCITY_TO_VOLUME_SINGLE = dp.knob(
    50, "cable"
)  # 0x3FFFFFE8, the stock sample-drum cable
CHOKE_ROWS = {"HH CLSD", "HH OPEN", "HH PEDAL"}
SIDECHAIN_ROWS = {"KICK"}


def layer_file(index, name, k, n):
    if n == 1:
        return f"{SD_SAMPLE_DIR}/{index:02d} {name}.wav"
    return f"{SD_SAMPLE_DIR}/{index:02d} {name} v{k}.wav"


def ensure_placeholders(root, files):
    """Real libraries: stage the converted WAVs here yourself. Otherwise make a ladder of
    sine bursts whose peaks ascend 6 dB per layer, so the ladder check has real levels."""
    made = 0
    for row_files in files:
        n = len(row_files)
        for k, rel in enumerate(row_files):
            p = os.path.join(root, rel)
            if os.path.isfile(p):
                continue
            dp.write_tone_wav(
                p, peak_dbfs=-6.0 * (n - k), seconds=0.05, freq=220.0 * (k + 1)
            )
            made += 1
    if made:
        print(
            f"wrote {made} placeholder WAVs under {os.path.join(root, SD_SAMPLE_DIR)}"
        )


def build(root):
    kit = dp.Kit(mod_fx_type="none")
    all_files = []
    for i, (name, group, bands) in enumerate(ROWS, start=1):
        files = [layer_file(i, name, k, len(bands)) for k in range(1, len(bands) + 1)]
        all_files.append(files)
        s = kit.drum(
            name,
            polyphonic="choke" if name in CHOKE_ROWS else "auto",
            side_chain_send=2147483647 if name in SIDECHAIN_ROWS else None,
        )
        if len(bands) == 1:
            s.osc1.set_sample(files[0], loop_mode=dp.LOOP_MODE_ONCE)
        else:
            s.osc1.set_ranges(
                [dp.SampleRange(f, top=t) for f, t in zip(files, bands)],
                keyed="velocity",
                loop_mode=dp.LOOP_MODE_ONCE,
            )
        randvol, randpitch = HUMANISE[group]
        s.cables = []  # replace the stock velocity->volume cable with the calibrated set
        s.cable(
            "velocity",
            "volume",
            VELOCITY_TO_VOLUME_SINGLE
            if len(bands) == 1
            else VELOCITY_TO_VOLUME_LAYERED,
        )
        s.cable("random", "volume", randvol)
        s.cable("random", "pitch", randpitch)
    ensure_placeholders(root, all_files)
    return kit


def main():
    root = staging_root()
    kit = build(root)
    path = os.path.join(root, "KITS", KIT_NAME + ".XML")
    rep = kit.validate(root)  # measures each staged WAV and checks the ladders ascend
    kit.write(path)
    report(rep, path)


if __name__ == "__main__":
    main()
