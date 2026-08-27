#!/usr/bin/env python3
"""Generate the example kits for the Plaits drum-model oscillator (OscType::DRUM).

Writes KITS/808 Models.XML and KITS/909 Models.XML next to this script. Copy them to the
KITS folder of the SD card. Every row is a synth drum using the `drum` oscillator type, so the
kits need no samples - just the EnableDrumModels community feature to be editable (they load and
play regardless).

Row parameters are the three Plaits macros in 0..1:
  tone  -> oscAPulseWidth       (Plaits TIMBRE, "half precision" 0..0x7FFFFFFF)
  decay -> oscAWavetablePosition (Plaits MORPH, full range)
  snap  -> carrier1Feedback     (Plaits HARMONICS: drive / snappy / noise, full range)
and `transpose` in semitones relative to the kit's default note (C3), remembering that the kick
models already play two octaves down and the snare models one octave down.
"""

from pathlib import Path


def half(v: float) -> str:
    """0..1 -> 0x00000000..0x7FFFFFFF (pulse width style param)."""
    return f"0x{round(max(0.0, min(1.0, v)) * 0x7FFFFFFF):08X}"


def full(v: float) -> str:
    """0..1 -> 0x80000000..0x7FFFFFFF (standard bipolar param)."""
    raw = round(max(0.0, min(1.0, v)) * 0xFFFFFFFF) - 0x80000000
    return f"0x{raw & 0xFFFFFFFF:08X}"


ROW_TEMPLATE = """\t\t<sound>
\t\t\t<name>{name}</name>
\t\t\t<osc1>
\t\t\t\t<type>drum</type>
\t\t\t\t<drumModel>{model}</drumModel>
\t\t\t\t<transpose>{transpose}</transpose>
\t\t\t\t<cents>0</cents>
\t\t\t\t<retrigPhase>-1</retrigPhase>
\t\t\t</osc1>
\t\t\t<osc2>
\t\t\t\t<type>square</type>
\t\t\t\t<transpose>0</transpose>
\t\t\t\t<cents>0</cents>
\t\t\t\t<retrigPhase>-1</retrigPhase>
\t\t\t</osc2>
\t\t\t<polyphonic>{polyphonic}</polyphonic>
\t\t\t<clippingAmount>0</clippingAmount>
\t\t\t<voicePriority>1</voicePriority>
\t\t\t<sideChainSend>2147483647</sideChainSend>
\t\t\t<lfo1>
\t\t\t\t<type>sine</type>
\t\t\t\t<syncLevel>0</syncLevel>
\t\t\t</lfo1>
\t\t\t<lfo2>
\t\t\t\t<type>sine</type>
\t\t\t</lfo2>
\t\t\t<mode>subtractive</mode>
\t\t\t<unison>
\t\t\t\t<num>1</num>
\t\t\t\t<detune>8</detune>
\t\t\t</unison>
\t\t\t<delay>
\t\t\t\t<pingPong>1</pingPong>
\t\t\t\t<analog>0</analog>
\t\t\t\t<syncLevel>7</syncLevel>
\t\t\t</delay>
\t\t\t<lpfMode>24dB</lpfMode>
\t\t\t<modFXType>none</modFXType>
\t\t\t<defaultParams>
\t\t\t\t<arpeggiatorGate>0x00000000</arpeggiatorGate>
\t\t\t\t<portamento>0x80000000</portamento>
\t\t\t\t<compressorShape>0xDC28F5B2</compressorShape>
\t\t\t\t<oscAVolume>0x7FFFFFFF</oscAVolume>
\t\t\t\t<oscAPulseWidth>{tone}</oscAPulseWidth>
\t\t\t\t<oscAWavetablePosition>{decay}</oscAWavetablePosition>
\t\t\t\t<oscBVolume>0x80000000</oscBVolume>
\t\t\t\t<oscBPulseWidth>0x00000000</oscBPulseWidth>
\t\t\t\t<noiseVolume>0x80000000</noiseVolume>
\t\t\t\t<volume>{volume}</volume>
\t\t\t\t<pan>{pan}</pan>
\t\t\t\t<lpfFrequency>0x7FFFFFFF</lpfFrequency>
\t\t\t\t<lpfResonance>0x00000000</lpfResonance>
\t\t\t\t<hpfFrequency>0x80000000</hpfFrequency>
\t\t\t\t<hpfResonance>0x80000000</hpfResonance>
\t\t\t\t<envelope1>
\t\t\t\t\t<attack>0x80000000</attack>
\t\t\t\t\t<decay>0xE6666654</decay>
\t\t\t\t\t<sustain>0x7FFFFFD2</sustain>
\t\t\t\t\t<release>0x80000000</release>
\t\t\t\t</envelope1>
\t\t\t\t<envelope2>
\t\t\t\t\t<attack>0xE6666654</attack>
\t\t\t\t\t<decay>0xE6666654</decay>
\t\t\t\t\t<sustain>0xFFFFFFE9</sustain>
\t\t\t\t\t<release>0xE6666654</release>
\t\t\t\t</envelope2>
\t\t\t\t<lfo1Rate>0x1999997E</lfo1Rate>
\t\t\t\t<lfo2Rate>0x00000000</lfo2Rate>
\t\t\t\t<modulator1Amount>0x80000000</modulator1Amount>
\t\t\t\t<modulator1Feedback>0x80000000</modulator1Feedback>
\t\t\t\t<modulator2Amount>0x80000000</modulator2Amount>
\t\t\t\t<modulator2Feedback>0x80000000</modulator2Feedback>
\t\t\t\t<carrier1Feedback>{snap}</carrier1Feedback>
\t\t\t\t<carrier2Feedback>0x80000000</carrier2Feedback>
\t\t\t\t<modFXRate>0x00000000</modFXRate>
\t\t\t\t<modFXDepth>0x00000000</modFXDepth>
\t\t\t\t<delayRate>0x00000000</delayRate>
\t\t\t\t<delayFeedback>0x80000000</delayFeedback>
\t\t\t\t<reverbAmount>0x80000000</reverbAmount>
\t\t\t\t<arpeggiatorRate>0x00000000</arpeggiatorRate>
\t\t\t\t<patchCables>
\t\t\t\t\t<patchCable>
\t\t\t\t\t\t<source>velocity</source>
\t\t\t\t\t\t<destination>volume</destination>
\t\t\t\t\t\t<amount>0x20000000</amount>
\t\t\t\t\t</patchCable>
\t\t\t\t</patchCables>
\t\t\t\t<stutterRate>0x00000000</stutterRate>
\t\t\t\t<sampleRateReduction>0x80000000</sampleRateReduction>
\t\t\t\t<bitCrush>0x80000000</bitCrush>
\t\t\t\t<equalizer>
\t\t\t\t\t<bass>0x00000000</bass>
\t\t\t\t\t<treble>0x00000000</treble>
\t\t\t\t\t<bassFrequency>0x00000000</bassFrequency>
\t\t\t\t\t<trebleFrequency>0x00000000</trebleFrequency>
\t\t\t\t</equalizer>
\t\t\t\t<modFXOffset>0x00000000</modFXOffset>
\t\t\t\t<modFXFeedback>0x00000000</modFXFeedback>
\t\t\t</defaultParams>
\t\t\t<midiKnobs>
\t\t\t</midiKnobs>
\t\t\t<modKnobs>
\t\t\t\t<modKnob><controlsParam>pan</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>volumePostFX</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>lpfResonance</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>lpfFrequency</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>env1Release</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>env1Attack</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>delayFeedback</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>delayRate</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>reverbAmount</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>volumePostReverbSend</controlsParam><patchAmountFromSource>compressor</patchAmountFromSource></modKnob>
\t\t\t\t<modKnob><controlsParam>pitch</controlsParam><patchAmountFromSource>lfo1</patchAmountFromSource></modKnob>
\t\t\t\t<modKnob><controlsParam>lfo1Rate</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>pitch</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>stutterRate</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>bitcrushAmount</controlsParam></modKnob>
\t\t\t\t<modKnob><controlsParam>sampleRateReduction</controlsParam></modKnob>
\t\t\t</modKnobs>
\t\t</sound>
"""

KIT_TEMPLATE = """<?xml version="1.0" encoding="UTF-8"?>
<kit>
\t<lpfMode>24dB</lpfMode>
\t<modFXType>none</modFXType>
\t<modFXCurrentParam>feedback</modFXCurrentParam>
\t<currentFilterType>lpf</currentFilterType>
\t<defaultParams>
\t\t<delay>
\t\t\t<rate>0x00000000</rate>
\t\t\t<feedback>0x80000000</feedback>
\t\t</delay>
\t\t<reverbAmount>0x80000000</reverbAmount>
\t\t<volume>0x3504F334</volume>
\t\t<pan>0x00000000</pan>
\t\t<lpf>
\t\t\t<frequency>0x7FFFFFFF</frequency>
\t\t\t<resonance>0x00000000</resonance>
\t\t</lpf>
\t\t<hpf>
\t\t\t<frequency>0x80000000</frequency>
\t\t\t<resonance>0xC0000000</resonance>
\t\t</hpf>
\t\t<modFXDepth>0x00000000</modFXDepth>
\t\t<modFXRate>0xE0000000</modFXRate>
\t\t<stutterRate>0x00000000</stutterRate>
\t\t<sampleRateReduction>0x80000000</sampleRateReduction>
\t\t<bitCrush>0x80000000</bitCrush>
\t\t<equalizer>
\t\t\t<bass>0x00000000</bass>
\t\t\t<treble>0x00000000</treble>
\t\t\t<bassFrequency>0x00000000</bassFrequency>
\t\t\t<trebleFrequency>0x00000000</trebleFrequency>
\t\t</equalizer>
\t\t<modFXOffset>0x00000000</modFXOffset>
\t\t<modFXFeedback>0x80000000</modFXFeedback>
\t</defaultParams>
\t<soundSources>
{rows}\t</soundSources>
</kit>
"""


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
):
    return ROW_TEMPLATE.format(
        name=name,
        model=model,
        transpose=transpose,
        tone=half(tone),
        decay=full(decay),
        snap=full(snap),
        volume=volume,
        pan=f"0x{round(pan * 0x7FFFFFFF) & 0xFFFFFFFF:08X}",
        polyphonic="choke" if choke else "0",
    )


# Rows are listed bottom-up on the grid, so the kick goes first.
# Tuning notes (kit default note C3 = 261.6 Hz): kicks play at note/4 (65 Hz), snares at note/2
# (131 Hz) - transposes below pull each row into the classic register. The hi-hat rows were fitted
# against real 808 hat samples: the model needs Tone ~0.95 (band-pass up near 8 kHz) and f0 ~414 Hz
# (transpose +8) to put its energy at 4-16 kHz like the real thing; lower Tone sounds like a bell.
# The cymbal / ride rows use the ring-modulated "hihat2" model, which gets closer to a real cymbal.
KIT_808 = [
    row("KICK", "808kick", tone=0.35, decay=0.55, snap=0.30, transpose=-3),
    row("KICK LONG", "808kick", tone=0.25, decay=0.85, snap=0.15, transpose=-5),
    row("SNARE", "808snare", tone=0.3, decay=0.3, snap=0.5, transpose=5),
    row("SNARE SNAP", "808snare", tone=0.15, decay=0.3, snap=0.65, transpose=5),
    row("TOM LO", "808snare", tone=0.20, decay=0.60, snap=0.00, transpose=-7),
    row("TOM MID", "808snare", tone=0.25, decay=0.55, snap=0.00, transpose=-1),
    row("TOM HI", "808snare", tone=0.30, decay=0.50, snap=0.00, transpose=4),
    row(
        "HAT CLOSED",
        "hihat",
        tone=0.95,
        decay=0.15,
        snap=0.15,
        transpose=8,
        choke=True,
    ),
    row("HAT OPEN", "hihat", tone=0.95, decay=0.65, snap=0.3, transpose=8, choke=True),
    row("CYMBAL", "hihat2", tone=0.9, decay=0.9, snap=0.7, transpose=-5),
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
        "HAT CLOSED",
        "hihat",
        tone=0.95,
        decay=0.15,
        snap=0.45,
        transpose=8,
        choke=True,
    ),
    row("HAT OPEN", "hihat", tone=0.95, decay=0.6, snap=0.45, transpose=8, choke=True),
    row("RIDE", "hihat2", tone=0.65, decay=0.9, snap=0.3, transpose=16),
]


def main():
    out = Path(__file__).resolve().parent / "KITS"
    out.mkdir(exist_ok=True)
    for name, rows in (("808 Models", KIT_808), ("909 Models", KIT_909)):
        path = out / f"{name}.XML"
        path.write_text(KIT_TEMPLATE.format(rows="".join(rows)))
        print(f"wrote {path} ({len(rows)} rows)")


if __name__ == "__main__":
    main()
