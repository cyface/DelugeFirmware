#!/usr/bin/env python3
"""deluge_preset.py - build Deluge SYNTH (<sound>) and KIT (<kit>) preset XML.

Stdlib only. One module for everything the four one-off generators used to re-derive:

    encoders        half() / full() / knob() / db() / semitones() / cents()
    envelopes       release_seconds() / decay_seconds() / attack_seconds() go through
                    the real table math (combineCablesExp -> lookupReleaseRate -> Envelope)
    builders        Sound, Osc, SampleRange, sample_ranges(), Kit  -> to_xml() / write()
    patch cables    PatchCable + Patcher: a port of sim_patcher_math.py that mirrors
                    patcher.cpp (cableToLinearParam, combineCablesLinear/Exp, paramRanges)
                    and functions.cpp (getFinalParameterValueVolume/Linear/Hybrid/Exp)
    validate()      ascending velocity ladders, unique topNotes, files present under a
                    staging root, param hex in range, cables that silence an oscillator

Every firmware fact is cited to the source file it came from (paths relative to
src/deluge/). Element order and attribute names follow the firmware's own serializer
(Sound::writeToFile, Kit::writeToFile, GlobalEffectable::write*ToFile,
ModControllableAudio::write*ToFile, PatchCableSet::writePatchCablesToFile), so a file
this module writes has the same shape the device writes when it saves a preset.
"""

import array
import itertools
import math
import os
import re
import struct
import wave

# --------------------------------------------------------------------------- constants

#: Serializer::writeFirmwareVersion writes kFirmwareVersionStringShort, which is
#: "c" + PROJECT_VERSION from the top-level CMakeLists.txt (version/CMakeLists.txt).
FIRMWARE_VERSION = "c1.3.0"
#: Output::writeToFile / SoundDrum::writeToFileAsInstrument (processing/sound/sound_drum.cpp)
EARLIEST_COMPATIBLE_FIRMWARE = "4.1.0-alpha"

INT32_MAX = 2147483647
INT32_MIN = -2147483648
ONE = 536870912  # patcher "1" (patcher.cpp: "536870912 counts as 1")
SAMPLE_RATE = 44100  # kSampleRate (definitions_cxx.hpp)
ENVELOPE_POS_END = 8388608  # Envelope::render: a stage ends when pos >= 8388608

#: Kit drums always sound this note (SoundDrum::noteOn -> kNoteForDrum).
NOTE_FOR_DRUM = 60
#: MultiRange::topNote default; a range that omits its bound gets this.
RANGE_TOP_DEFAULT = 32767

LOOP_MODE_CUT = 0  # SampleRepeatMode::CUT  (definitions_cxx.hpp) - note-off damps
LOOP_MODE_ONCE = 1  # SampleRepeatMode::ONCE - one-shot, note-off ignored
LOOP_MODE_LOOP = 2  # SampleRepeatMode::LOOP
LOOP_MODE_STRETCH = 3  # SampleRepeatMode::STRETCH

OSC_TYPES = (
    "square",
    "saw",
    "analogSaw",
    "analogSquare",
    "sine",
    "triangle",
    "sample",
    "wavetable",
    "inLeft",
    "inRight",
    "inStereo",
    "dx7",
    "drum",
)
#: Plaits models for type="drum" (util/functions.cpp drumModelToString). FORK ONLY.
DRUM_MODELS = ("808kick", "808snare", "hihat", "909kick", "909snare", "hihat2")
LFO_TYPES = ("square", "saw", "sine", "sah", "rwalk", "warbler", "triangle")
SYNTH_MODES = ("subtractive", "fm", "ringmod")
POLYPHONY_MODES = ("auto", "poly", "mono", "legato", "choke")
MOD_FX_TYPES = (
    "none",
    "flanger",
    "TapeWarble",
    "dimension",
    "chorus",
    "StereoChorus",
    "grainFX",
    "phaser",
)
FILTER_MODES = ("12dB", "24dB", "24dBDrive", "SVF_Band", "SVF_Notch", "HPLadder", "Off")
FILTER_ROUTES = ("H2L", "L2H", "PARA")  # filters/filter_config.cpp routeMap
PATCH_SOURCES = (
    "lfo1",
    "lfo2",
    "lfo3",
    "lfo4",
    "envelope1",
    "envelope2",
    "envelope3",
    "envelope4",
    "velocity",
    "note",
    "compressor",
    "random",
    "aftertouch",
    "x",
    "y",
)

#: Attributes / values only the cyface fork understands. Stock firmware behaviour:
#:   drumModel + type="drum": stringToOscType("drum") falls back to TRIANGLE - the row
#:                           loads and plays a triangle wave (silent fallback).
#:   rangeTopVelocity:       the stock reader does not know the name, so every range keeps
#:                           topNote 32767, the second one trips the duplicate check and
#:                           Sound::readTagFromFile returns FILE_CORRUPTED (shown as
#:                           "SD card error"). NOT a silent fallback.
#:   tapeSaturation:         unknown defaultParams attribute, skipped (silent).
FORK_ONLY = {"drumModel", "rangeTopVelocity", "tapeSaturation"}

# ------------------------------------------------------------------- fixed-point helpers


def s32(x):
    """Interpret a 32-bit pattern (or a hex string) as a signed int32."""
    if isinstance(x, str):
        x = int(x, 16)
    x &= 0xFFFFFFFF
    return x - 0x100000000 if x >= 0x80000000 else x


def hex32(x):
    """Signed/unsigned int -> the 0xXXXXXXXX form the preset files use."""
    return "0x%08X" % (int(x) & 0xFFFFFFFF)


def _sat32(x):
    return max(INT32_MIN, min(INT32_MAX, x))


def _mul32_rshift32(a, b):
    """multiply_32x32_rshift32 - 64-bit product, top 32 bits (arithmetic)."""
    return (a * b) >> 32


def _lshift_sat(x, n):
    """lshiftAndSaturate<n>"""
    return _sat32(x << n)


def _clamp01(v):
    return max(0.0, min(1.0, float(v)))


# ---------------------------------------------------------------------------- encoders


def half(v):
    """0..1 -> 0x00000000..0x7FFFFFFF. For UNIPOLAR params (oscAPulseWidth / oscBPulseWidth).

    getParamFromUserValue (util/functions.cpp) treats the phase-width params as unsigned:
    userValue * (85899345 >> 1), i.e. knob 50 = 0x7FFFFFE9. This is the continuous form.
    """
    return hex32(round(_clamp01(v) * 0x7FFFFFFF))


def full(v):
    """0..1 -> 0x80000000..0x7FFFFFFF. For the standard BIPOLAR params."""
    return hex32(round(_clamp01(v) * 0xFFFFFFFF) - 0x80000000)


def knob(n, kind="bipolar"):
    """Menu/knob value -> preset hex, exactly as getParamFromUserValue (util/functions.cpp).

    kind = "bipolar"     0..50   userValue * 85899345 - 2147483648 (the default case;
                                  knob 50 = 0x7FFFFFD2, which is what the device writes)
           "pulseWidth"  0..50   userValue * (85899345 >> 1), unsigned (phase width params)
           "cable"     -50..50   userValue * 21474836 (PATCH_CABLE amounts; 50 = 0x3FFFFFE8)
           "eq"        -50..50   bass / treble: 0 -> 0, -50 -> 0x80000000, else * 42949672
    A value written this way can be nudged on the device and re-saved without drifting.
    """
    n = int(n)
    if kind == "bipolar":
        if not 0 <= n <= 50:
            raise ValueError(f"bipolar knob must be 0..50, got {n!r}")
        return hex32(n * 85899345 - 2147483648)
    if kind == "pulseWidth":
        if not 0 <= n <= 50:
            raise ValueError(f"pulseWidth knob must be 0..50, got {n!r}")
        return hex32(n * (85899345 >> 1))
    if kind == "cable":
        if not -50 <= n <= 50:
            raise ValueError(f"cable knob must be -50..50, got {n!r}")
        return hex32(n * 21474836)
    if kind == "eq":
        if not -50 <= n <= 50:
            raise ValueError(f"eq knob must be -50..50, got {n!r}")
        if n == -50:
            return hex32(INT32_MIN)
        return hex32(n * 42949672)
    raise ValueError(f"unknown knob kind {kind!r}")


def db(d):
    """dB below full scale -> preset hex for a VOLUME-class param.

    Volume-class params (volume, oscAVolume, oscBVolume, noiseVolume, reverbAmount,
    modulator amounts) are squared on the way out (getFinalParameterValueVolume,
    util/functions.cpp): with no cables the gain is ((preset/2^31 + 1) / 2)^2 of full
    scale, so amplitude a needs preset fraction sqrt(a). 0 dB -> 0x7FFFFFFF,
    -12 dB -> ~0x00418937 (near the 0x00000000 centre), -40 dB -> ~0x8CCCCCCD.
    """
    if d > 0:
        raise ValueError("db() is relative to full scale; use 0 or negative dB")
    return full(10 ** (d / 40.0))


def db_of(preset):
    """Inverse of db(): the unpatched gain of a volume-class preset value, in dB."""
    frac = (s32(preset) + 0x80000000) / 0xFFFFFFFF
    if frac <= 0:
        return -math.inf
    return 40.0 * math.log10(frac)


#: Patched-value units per semitone for the pitch destinations: getExp (util/functions.cpp)
#: doubles the pitch every 2^26 of patched value, and combineCablesExp scales the preset by
#: paramRanges[pitch] = 536870912 = 2^29 (i.e. /8), so one octave of PRESET is 2^29.
SEMITONE_PRESET = (1 << 29) / 12.0
SEMITONE_PATCHED = (1 << 26) / 12.0
CENT_PATCHED = SEMITONE_PATCHED / 100.0  # 55924 - matches the calibrated value


def semitones(n):
    """Semitone offset -> preset hex for the exp pitch params (pitchAdjust, oscAPitchAdjust,
    oscBPitchAdjust, mod1PitchAdjust, mod2PitchAdjust). +/-48 semitones is full scale.

    NOTE: this is the *preset* space. Cable amounts to the pitch destinations are squared
    first (PatchCableSet::getModifiedPatchCableAmount) - use pitch_cable_amount() for those.
    """
    return hex32(_sat32(round(n * SEMITONE_PRESET)))


def cents(n):
    """Cent offset -> preset hex for the exp pitch params (see semitones())."""
    return semitones(n / 100.0)


# ------------------------------------------------------------------- param bookkeeping

#: Attribute order of <defaultParams ...> exactly as Sound::writeParamsToFile writes it
#: (processing/sound/sound.cpp), followed by ModControllableAudio::writeParamAttributesToFile
#: (model/mod_controllable/mod_controllable_audio.cpp) and the community params.
#: (name, default hex). pitchAdjust and friends are only written when non-default, so they
#: are not in this list; set them with Sound.set() if you need them.
SOUND_PARAM_DEFAULTS = (
    ("portamento", "0x80000000"),
    ("compressorShape", "0xDC28F5B2"),
    ("oscAVolume", "0x7FFFFFFF"),
    ("oscAPulseWidth", "0x00000000"),
    ("oscAWavetablePosition", "0x00000000"),
    ("oscBVolume", "0x80000000"),
    ("oscBPulseWidth", "0x00000000"),
    ("oscBWavetablePosition", "0x00000000"),
    ("noiseVolume", "0x80000000"),
    ("volume", "0x4CCCCCA8"),  # knob 40
    ("pan", "0x00000000"),
    ("lpfFrequency", "0x7FFFFFFF"),
    ("lpfResonance", "0x80000000"),
    ("hpfFrequency", "0x80000000"),
    ("hpfResonance", "0x80000000"),
    ("lfo1Rate", "0x1999997E"),
    ("lfo2Rate", "0x00000000"),
    ("lfo3Rate", "0x1999997E"),
    ("lfo4Rate", "0x00000000"),
    ("modulator1Amount", "0x80000000"),
    ("modulator1Feedback", "0x80000000"),
    ("modulator2Amount", "0x80000000"),
    ("modulator2Feedback", "0x80000000"),
    ("carrier1Feedback", "0x80000000"),
    ("carrier2Feedback", "0x80000000"),
    ("modFXRate", "0x00000000"),
    ("modFXDepth", "0x00000000"),
    ("delayRate", "0x00000000"),
    ("delayFeedback", "0x80000000"),
    ("reverbAmount", "0x80000000"),
    ("arpeggiatorRate", "0x00000000"),
    # ModControllableAudio::writeParamAttributesToFile
    ("stutterRate", "0x00000000"),
    ("sampleRateReduction", "0x80000000"),
    ("bitCrush", "0x80000000"),
    ("modFXOffset", "0x00000000"),
    ("modFXFeedback", "0x00000000"),
    ("compressorThreshold", "0x00000000"),
    ("arpeggiatorGate", "0x00000000"),
    ("noteProbability", "0x7FFFFFFF"),
    ("bassProbability", "0x80000000"),
    ("swapProbability", "0x80000000"),
    ("glideProbability", "0x80000000"),
    ("reverseProbability", "0x80000000"),
    ("chordProbability", "0x80000000"),
    ("ratchetProbability", "0x80000000"),
    ("ratchetAmount", "0x80000000"),
    ("sequenceLength", "0x80000000"),
    ("chordPolyphony", "0x80000000"),
    ("rhythm", "0x80000000"),
    ("spreadVelocity", "0x80000000"),
    ("spreadGate", "0x80000000"),
    ("spreadOctave", "0x80000000"),
    # community params, written last (Sound::writeParamsToFile)
    ("lpfMorph", "0x80000000"),
    ("hpfMorph", "0x80000000"),
    ("waveFold", "0x80000000"),
)
SOUND_PARAM_NAMES = tuple(n for n, _ in SOUND_PARAM_DEFAULTS)
#: Optional attributes the writer emits only when they hold something (in writer order,
#: they slot in right after carrier2Feedback).
SOUND_OPTIONAL_PARAMS = (
    "pitchAdjust",
    "oscAPitchAdjust",
    "oscBPitchAdjust",
    "mod1PitchAdjust",
    "mod2PitchAdjust",
    "tapeSaturation",
)
#: Unipolar preset params: 0x00000000..0x7FFFFFFF (getParamFromUserValue treats phase
#: width as unsigned). Everything else is bipolar 0x80000000..0x7FFFFFFF.
UNIPOLAR_PARAMS = frozenset({"oscAPulseWidth", "oscBPulseWidth"})

#: Envelope attribute order (Sound::writeParamsToFile).
ENVELOPE_FIELDS = ("attack", "decay", "sustain", "release")
ENVELOPE_DEFAULTS = (
    {
        "attack": "0x80000000",
        "decay": "0xE6666654",
        "sustain": "0x7FFFFFD2",
        "release": "0x80000000",
    },
    {
        "attack": "0xE6666654",
        "decay": "0xE6666654",
        "sustain": "0xFFFFFFE9",
        "release": "0xE6666654",
    },
    {
        "attack": "0x00000000",
        "decay": "0x00000000",
        "sustain": "0x00000000",
        "release": "0x00000000",
    },
    {
        "attack": "0x00000000",
        "decay": "0x00000000",
        "sustain": "0x00000000",
        "release": "0x00000000",
    },
)
EQUALIZER_DEFAULTS = (
    ("bass", "0x00000000"),
    ("treble", "0x00000000"),
    ("bassFrequency", "0x00000000"),
    ("trebleFrequency", "0x00000000"),
)

#: The 16 gold-knob assignments the device writes for a fresh sound.
DEFAULT_MOD_KNOBS = (
    ("pan", None),
    ("volumePostFX", None),
    ("lpfResonance", None),
    ("lpfFrequency", None),
    ("env1Release", None),
    ("env1Attack", None),
    ("delayFeedback", None),
    ("delayRate", None),
    ("reverbAmount", None),
    ("volumePostReverbSend", "compressor"),
    ("pitch", "lfo1"),
    ("lfo1Rate", None),
    ("portamento", None),
    ("stutterRate", None),
    ("bitcrushAmount", None),
    ("sampleRateReduction", None),
)

#: Kit-level <defaultParams> (GlobalEffectable::writeParamAttributesToFile then
#: ModControllableAudio::writeParamAttributesToFile then the community params).
KIT_PARAM_DEFAULTS = (
    ("reverbAmount", "0x80000000"),
    ("volume", "0x3504F334"),
    ("pan", "0x00000000"),
    ("sidechainCompressorShape", "0xDC28F5B2"),
    ("modFXDepth", "0x00000000"),
    ("modFXRate", "0xE0000000"),
    ("stutterRate", "0x00000000"),
    ("sampleRateReduction", "0x80000000"),
    ("bitCrush", "0x80000000"),
    ("modFXOffset", "0x00000000"),
    ("modFXFeedback", "0x80000000"),
    ("compressorThreshold", "0x00000000"),
    ("lpfMorph", "0x80000000"),
    ("hpfMorph", "0x80000000"),
    ("arpeggiatorRate", "0x00000000"),
)
KIT_PARAM_NAMES = tuple(n for n, _ in KIT_PARAM_DEFAULTS)
KIT_PARAM_TAGS = (
    ("delay", (("rate", "0x00000000"), ("feedback", "0x80000000"))),
    ("lpf", (("frequency", "0x7FFFFFFF"), ("resonance", "0x00000000"))),
    ("hpf", (("frequency", "0x80000000"), ("resonance", "0xC0000000"))),
    ("equalizer", EQUALIZER_DEFAULTS),
)

# ------------------------------------------------------------ patcher param tables
# Which combine/finalise path each patch-cable DESTINATION takes. Names are the ones
# PatchCableSet::writePatchCablesToFile writes (params::paramNameForFile, PATCHED kind).
# Grouping follows the enum order in modulation/params/param.h:
#   [FIRST_LOCAL, FIRST_LOCAL_NON_VOLUME)  volume  (getFinalParameterValueVolume: squared)
#   [FIRST_LOCAL_NON_VOLUME, FIRST_HYBRID) linear  (getFinalParameterValueLinear)
#   [FIRST_HYBRID, FIRST_EXP)              hybrid  (getFinalParameterValueHybrid)
#   [FIRST_EXP, LOCAL_LAST)                exp     (getFinalParameterValueExp / envelope hack)
# and the same four bands again for the GLOBAL params.
DEST_KIND = {
    "oscAVolume": "volume",
    "oscBVolume": "volume",
    "volume": "volume",
    "noiseVolume": "volume",
    "modulator1Volume": "volume",
    "modulator2Volume": "volume",
    "waveFold": "volume",
    "volumePostFX": "volume",
    "volumePostReverbSend": "volume",
    "reverbAmount": "volume",
    "modFXDepth": "volume",
    "modulator1Feedback": "linear",
    "modulator2Feedback": "linear",
    "carrier1Feedback": "linear",
    "carrier2Feedback": "linear",
    "lpfResonance": "linear",
    "hpfResonance": "linear",
    "env1Sustain": "linear",
    "env2Sustain": "linear",
    "env3Sustain": "linear",
    "env4Sustain": "linear",
    "lpfMorph": "linear",
    "hpfMorph": "linear",
    "delayFeedback": "linear",
    "oscAPhaseWidth": "hybrid",
    "oscBPhaseWidth": "hybrid",
    "oscAWavetablePosition": "hybrid",
    "oscBWavetablePosition": "hybrid",
    "pan": "hybrid",
    "lpfFrequency": "exp",
    "pitch": "exp",
    "oscAPitch": "exp",
    "oscBPitch": "exp",
    "modulator1Pitch": "exp",
    "modulator2Pitch": "exp",
    "hpfFrequency": "exp",
    "lfo2Rate": "exp",
    "lfo4Rate": "exp",
    "env1Attack": "exp",
    "env2Attack": "exp",
    "env3Attack": "exp",
    "env4Attack": "exp",
    "env1Decay": "exp",
    "env2Decay": "exp",
    "env3Decay": "exp",
    "env4Decay": "exp",
    "env1Release": "exp",
    "env2Release": "exp",
    "env3Release": "exp",
    "env4Release": "exp",
    "delayRate": "exp",
    "modFXRate": "exp",
    "lfo1Rate": "exp",
    "lfo3Rate": "exp",
    "arpRate": "exp",
}
PATCH_DESTINATIONS = tuple(DEST_KIND)

#: Which <defaultParams> attribute holds the preset for a cable destination. Note the
#: trap: the attribute "volume" is GLOBAL_VOLUME_POST_FX, while the cable destination
#: "volume" is LOCAL_VOLUME, whose preset is fixed at 0 (Sound::initParams,
#: setCurrentValueBasicForSetup(0)) and never written to the file.
DEST_PRESET_ATTR = {
    "volume": None,
    "volumePostFX": "volume",
    "oscAPhaseWidth": "oscAPulseWidth",
    "oscBPhaseWidth": "oscBPulseWidth",
    "modulator1Volume": "modulator1Amount",
    "modulator2Volume": "modulator2Amount",
    "arpRate": "arpeggiatorRate",
    "pitch": "pitchAdjust",
    "oscAPitch": "oscAPitchAdjust",
    "oscBPitch": "oscBPitchAdjust",
    "modulator1Pitch": "mod1PitchAdjust",
    "modulator2Pitch": "mod2PitchAdjust",
    "volumePostReverbSend": None,
}
for _n in range(1, 5):
    for _f in ENVELOPE_FIELDS:
        DEST_PRESET_ATTR[f"env{_n}{_f.capitalize()}"] = (f"envelope{_n}", _f)


def param_range(dest):
    """paramRanges[p] - getParamRange (util/functions.cpp)."""
    if dest.startswith("env") and dest.endswith("Attack"):
        return int(536870912 * 1.5)
    if dest in (
        "delayRate",
        "pitch",
        "oscAPitch",
        "oscBPitch",
        "modulator1Pitch",
        "modulator2Pitch",
    ):
        return 536870912
    if dest == "lpfFrequency":
        return int(536870912 * 1.4)
    return 1073741824


def param_neutral(dest):
    """paramNeutralValues[p] - getParamNeutralValue (util/functions.cpp)."""
    if dest in (
        "oscAVolume",
        "oscBVolume",
        "volumePostReverbSend",
        "noiseVolume",
        "reverbAmount",
        "volumePostFX",
        "volume",
    ):
        return 134217728
    if dest in ("modulator1Volume", "modulator2Volume"):
        return 33554432
    if dest == "lpfFrequency":
        return 2000000
    if dest == "hpfFrequency":
        return 2672947
    if dest in ("lfo1Rate", "lfo2Rate", "lfo3Rate", "lfo4Rate", "modFXRate"):
        return 121739
    if dest in ("lpfResonance", "hpfResonance", "lpfMorph", "hpfMorph", "waveFold"):
        return 25 * 10737418
    if dest in ("pan", "oscAPhaseWidth", "oscBPhaseWidth"):
        return 0
    if dest.startswith("env"):
        if dest.endswith("Attack"):
            return 4096
        if dest.endswith("Release"):
            return 140 << 9
        if dest.endswith("Decay"):
            return 70 << 9
        if dest.endswith("Sustain"):
            return 1073741824
    if dest == "delayFeedback":
        return 1073741824
    if dest.endswith("Feedback"):
        return 5931642
    if dest in (
        "delayRate",
        "arpRate",
        "pitch",
        "oscAPitch",
        "oscBPitch",
        "modulator1Pitch",
        "modulator2Pitch",
    ):
        return INT32_MAX
    if dest == "modFXDepth":
        return 526133494
    return 0


# ------------------------------------------------------------------- envelope timing

#: util/lookuptables/lookuptables.cpp - indexed by lookupReleaseRate()
RELEASE_RATE_TABLE_64 = (
    1959518848,
    240577040,
    126456408,
    84972672,
    63518640,
    50408868,
    41567836,
    35202512,
    30400724,
    26649400,
    23637820,
    21166812,
    19102814,
    17352918,
    15850494,
    14546511,
    13404089,
    12394955,
    11497068,
    10692993,
    9968758,
    9313033,
    8716538,
    8171592,
    7671792,
    7211749,
    6786901,
    6393359,
    6027784,
    5687299,
    5369406,
    5071928,
    4792960,
    4530827,
    4284050,
    4051316,
    3831462,
    3623446,
    3426337,
    3239300,
    3061582,
    2892503,
    2731448,
    2577859,
    2431229,
    2291096,
    2157036,
    2028664,
    1905625,
    1787593,
    1674270,
    1565377,
    1460662,
    1359888,
    1262836,
    1169305,
    1079105,
    992062,
    908014,
    826807,
    748301,
    672363,
    598870,
    527703,
    458757,
)


def lookup_release_rate(patched):
    """lookupReleaseRate (util/functions.cpp): patched exp value -> per-sample rate."""
    magnitude = 24
    which = patched >> magnitude
    how_much_further = (patched << (31 - magnitude)) & INT32_MAX
    which += 32
    if which < 0:
        return RELEASE_RATE_TABLE_64[0]
    if which >= 64:
        return RELEASE_RATE_TABLE_64[64]
    v1 = RELEASE_RATE_TABLE_64[which]
    v2 = RELEASE_RATE_TABLE_64[which + 1]
    return (
        _mul32_rshift32(v2, how_much_further)
        + _mul32_rshift32(v1, INT32_MAX - how_much_further)
    ) << 1


def get_exp(preset_value, adjustment):
    """getExp (util/functions.cpp): preset * 2^(adjustment / 2^26).

    The firmware interpolates expTableSmall for the fractional doubling; this uses the
    closed form, which the table approximates to ~16 bits.
    """
    return _sat32(int(preset_value * 2.0 ** (adjustment / 67108864.0)))


def envelope_rate(stage, preset):
    """Per-sample envelope increment for a preset value, as Voice::render sees it.

    combineCablesExp folds the preset in as preset * paramRanges[p] >> 32 (patcher.cpp),
    then getFinalParameterValueExpWithDumbEnvelopeHack (util/functions.cpp) turns it into
    a rate: decay/release go through lookupReleaseRate x the neutral value, attack is
    negated and goes through getExp.
    """
    preset = s32(preset)
    dest = "env1" + stage.capitalize()
    patched = _mul32_rshift32(preset, param_range(dest))
    if stage in ("decay", "release"):
        return _mul32_rshift32(param_neutral(dest), lookup_release_rate(patched))
    if stage == "attack":
        return get_exp(param_neutral(dest), -patched)
    raise ValueError("stage must be attack, decay or release")


def envelope_seconds(stage, value):
    """Seconds an envelope stage takes for a knob value (0..50) or a preset hex/int.

    Envelope::render (modulation/envelope.cpp) adds the rate once per sample and ends the
    stage at pos >= 8388608. Attack rates above 245632 skip straight to decay (0 s).
    """
    preset = knob(value) if isinstance(value, int) and 0 <= value <= 50 else value
    rate = envelope_rate(stage, preset)
    if rate <= 0:
        return math.inf
    if stage == "attack" and rate > 245632:
        return 0.0
    return ENVELOPE_POS_END / rate / SAMPLE_RATE


def release_seconds(value):
    """Knob 6 = 0.36 s, 20 = 1.64 s, 30 = 3.46 s, 50 = 27 s (see envelope_seconds)."""
    return envelope_seconds("release", value)


def decay_seconds(value):
    return envelope_seconds("decay", value)


def attack_seconds(value):
    return envelope_seconds("attack", value)


def knob_seconds_table(stage="release"):
    """[(knob, seconds)] for knob 0..50 - handy when picking envelope values."""
    return [(k, envelope_seconds(stage, k)) for k in range(51)]


# ---------------------------------------------------------------------- patch cables


class PatchCable:
    """One <patchCable source= destination= polarity= amount= /> (PatchCableSet::writePatchCablesToFile)."""

    def __init__(self, source, destination, amount, polarity="bipolar"):
        if source not in PATCH_SOURCES:
            raise ValueError(
                "unknown patch source {!r} (want one of {})".format(
                    source, ", ".join(PATCH_SOURCES)
                )
            )
        if destination not in DEST_KIND:
            raise ValueError(f"unknown patch destination {destination!r}")
        if polarity not in ("bipolar", "unipolar"):
            raise ValueError("polarity must be bipolar or unipolar")
        self.source = source
        self.destination = destination
        self.amount = hex32(amount) if not isinstance(amount, str) else amount
        self.polarity = polarity
        _check_hex(self.amount, "patchCable amount")

    @property
    def amount_int(self):
        return s32(self.amount)

    def __repr__(self):
        return f"PatchCable({self.source!r}, {self.destination!r}, {self.amount!r})"

    def to_xml(self, indent):
        return '{}<patchCable source="{}" destination="{}" polarity="{}" amount="{}" />'.format(
            "\t" * indent, self.source, self.destination, self.polarity, self.amount
        )


class Patcher:
    """Simulation of patcher.cpp + util/functions.cpp for one destination.

    Ported from sim_patcher_math.py (SalamanderGrandPiano/Deluge). All arithmetic is the
    firmware's int32 arithmetic, so the numbers are the ones the device computes.
    """

    # -- source values (model/voice/voice.cpp) -------------------------------------
    @staticmethod
    def source_value(source, x):
        """Raw int32 source value.
        velocity / note: x is 0..127 (velocity 128 = INT32_MAX). Both are bipolar about 64:
            (x - 64) * 33554432                                      voice.cpp:132 / 161
        aftertouch: x is 0..1 (stored unipolar).
        everything else (lfo*, envelope*, random, compressor, x, y): x is -1..1.
        """
        if source in ("velocity", "note"):
            if x >= 128:
                return INT32_MAX
            return (int(x) - 64) * 33554432
        if source == "aftertouch":
            return int(_clamp01(x) * INT32_MAX)
        return int(max(-1.0, min(1.0, float(x))) * INT32_MAX)

    @staticmethod
    def to_polarity(cable, value):
        """PatchCable::toPolarity (modulation/patch/patch_cable.h). Only aftertouch converts."""
        if cable.source == "aftertouch" and cable.polarity == "bipolar":
            return (value - (INT32_MAX // 2)) * 2
        return value

    # -- patcher.cpp ----------------------------------------------------------------
    @staticmethod
    def cable_to_linear(running_total, source_value, cable_strength):
        """cableToLinearParam / ...WithoutRangeAdjustment (patcher.cpp).
        running_total *= (536870912 + source*strength) / 536870912, saturated at "4"."""
        scaled = _mul32_rshift32(source_value, cable_strength)
        made_positive = scaled + ONE
        return _lshift_sat(_mul32_rshift32(running_total, made_positive), 3)

    @staticmethod
    def cable_to_exp(running_total, source_value, cable_strength):
        """cableToExpParam (patcher.cpp): cables ADD for exp params."""
        return running_total + _mul32_rshift32(source_value, cable_strength)

    @classmethod
    def modified_cable_amount(cls, cable):
        """PatchCableSet::getModifiedPatchCableAmount: pitch destinations (and delayRate)
        SQUARE the amount; master pitch additionally scales by 2/3 (velocity) or 1/sqrt2."""
        amount = cable.amount_int
        p = cable.destination
        if p in (
            "pitch",
            "oscAPitch",
            "oscBPitch",
            "modulator1Pitch",
            "modulator2Pitch",
            "delayRate",
        ):
            out = (amount >> 15) * (amount >> 16)
            if amount < 0:
                out = -out
            if p == "pitch":
                k = 1431655765 if cable.source == "velocity" else 1518500250
                out = ((out * k + (1 << 31)) >> 32) << 1
            return out
        return amount

    @classmethod
    def combine(cls, destination, preset, cables, sources):
        """combineCablesLinear / combineCablesExp for `destination`.

        preset:  preset hex/int for the destination (LOCAL_VOLUME is always 0).
        cables:  PatchCables (only those aimed at `destination` are used).
        sources: {source_name: x} in the units of source_value(); missing = neutral 0
                 (velocity defaults to 64, i.e. the source's zero).
        Returns the patched value (before the final-value function).
        """
        preset = s32(preset)
        kind = DEST_KIND[destination]
        rng = param_range(destination)
        mine = [c for c in cables if c.destination == destination]

        def sv(c):
            default = 64 if c.source in ("velocity", "note") else 0
            return cls.to_polarity(
                c, cls.source_value(c.source, sources.get(c.source, default))
            )

        if kind in ("volume", "linear"):
            rt = ONE
            rt = cls.cable_to_linear(
                rt, preset, rng
            )  # the preset is treated as a cable
            for c in mine:
                rt = cls.cable_to_linear(rt, sv(c), c.amount_int)
            return rt - ONE
        rt = 0
        for c in mine:
            rt = cls.cable_to_exp(rt, sv(c), cls.modified_cable_amount(c))
        if destination in ("oscAWavetablePosition", "oscBWavetablePosition"):
            rt <<= 1
        return cls.cable_to_exp(rt, preset, rng)

    # -- util/functions.cpp ---------------------------------------------------------
    @staticmethod
    def final_volume(neutral, patched):
        """getFinalParameterValueVolume: squares (patched + 1), so "1" -> neutral, "2" -> 4x."""
        pos = patched + ONE
        pos = (pos >> 16) * (pos >> 15)
        return _lshift_sat(_mul32_rshift32(pos, neutral), 5)

    @staticmethod
    def final_linear(neutral, patched):
        return _lshift_sat(_mul32_rshift32(patched + ONE, neutral), 3)

    @staticmethod
    def final_hybrid(neutral, patched):
        pre = (neutral >> 2) + (patched >> 1)
        pre = max(-(1 << 28), min((1 << 28) - 1, pre))
        return pre << 2

    @classmethod
    def final_exp(cls, destination, neutral, patched):
        if destination.startswith("env") and (
            destination.endswith(("Decay", "Release"))
        ):
            return _mul32_rshift32(neutral, lookup_release_rate(patched))
        if destination.startswith("env") and destination.endswith("Attack"):
            patched = -patched
        return get_exp(neutral, patched)

    @classmethod
    def final_value(cls, destination, patched):
        kind = DEST_KIND[destination]
        neutral = param_neutral(destination)
        if kind == "volume":
            return cls.final_volume(neutral, patched)
        if kind == "linear":
            return cls.final_linear(neutral, patched)
        if kind == "hybrid":
            return cls.final_hybrid(neutral, patched)
        return cls.final_exp(destination, neutral, patched)

    @classmethod
    def evaluate(cls, destination, preset, cables, sources):
        """The int32 paramFinalValues[destination] the voice would use."""
        return cls.final_value(
            destination, cls.combine(destination, preset, cables, sources)
        )

    @classmethod
    def gain_db(cls, destination, preset, cables, sources):
        """For volume-class destinations: gain in dB relative to full scale (preset max, no cables)."""
        if DEST_KIND[destination] != "volume":
            raise ValueError("gain_db is for volume-class destinations")
        full_scale = 4 * param_neutral(destination)
        v = cls.evaluate(destination, preset, cables, sources)
        if v <= 0:
            return -math.inf
        return 20.0 * math.log10(v / full_scale)

    @classmethod
    def pitch_semitones(cls, destination, preset, cables, sources):
        """For the pitch destinations: the pitch offset in semitones the patcher produces."""
        patched = cls.combine(destination, preset, cables, sources)
        return patched / SEMITONE_PATCHED

    @classmethod
    def velocity_sweep(cls, destination, preset, cables, other_sources=None):
        """[(velocity, final_value)] for velocity 1..127 with the other sources fixed."""
        base = dict(other_sources or {})
        out = []
        for v in range(1, 128):
            base["velocity"] = v
            out.append((v, cls.evaluate(destination, preset, cables, base)))
        return out

    @classmethod
    def silent_velocities(
        cls, destination, preset, cables, other_sources=None, ratio=100
    ):
        """Velocities (1..127) at which the destination is at or below peak/ratio.

        This is the "cable silences the osc" check: with the multiplicative linear maths a
        full-scale cable drives (536870912 + contribution) through zero mid-sweep, and a
        preset of 0x80000000 makes the running total zero before any cable is applied.
        """
        rows = cls.velocity_sweep(destination, preset, cables, other_sources)
        peak = max(v for _, v in rows)
        if peak <= 0:
            return [v for v, _ in rows]
        return [v for v, val in rows if val <= peak // ratio]


def pitch_cable_amount(semis, source="random", destination="pitch"):
    """Cable amount hex that gives +/- `semis` semitones at the source's extreme.

    Inverts getModifiedPatchCableAmount (squared, then 2/3 for velocity->pitch or 1/sqrt2
    for anything else ->pitch) and the 2^26-per-octave exp scale. E.g. +/-9.4 cents of
    random->pitch humanisation is pitch_cable_amount(0.094) = ~0x035E8000.
    """
    target = semis * SEMITONE_PATCHED  # patched units at source = INT32_MAX
    if destination == "pitch":
        target /= (2.0 / 3.0) if source == "velocity" else (1.0 / math.sqrt(2.0))
    elif destination not in (
        "oscAPitch",
        "oscBPitch",
        "modulator1Pitch",
        "modulator2Pitch",
        "delayRate",
    ):
        raise ValueError("pitch_cable_amount is for the pitch destinations")
    # source * strength >> 32 with source = INT32_MAX ~= 2^31: strength ~= 2 * target
    strength = 2.0 * abs(target)
    # strength = (A>>15)*(A>>16) ~= A^2 / 2^31
    amount = math.sqrt(strength * (1 << 31))
    return hex32(round(math.copysign(amount, semis)))


# ------------------------------------------------------------------------- XML utils

_HEX_RE = re.compile(r"^0x[0-9A-Fa-f]{8}$")


class PresetError(ValueError):
    pass


def _check_hex(value, what):
    if not isinstance(value, str) or not _HEX_RE.match(value):
        raise PresetError(f"{what} must be an 8-digit 0x hex string, got {value!r}")


def _esc(s):
    return (
        str(s)
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def _tag_inline(indent, name, attrs):
    body = " ".join(f'{k}="{_esc(v)}"' for k, v in attrs)
    return "{}<{} {} />".format("\t" * indent, name, body)


def _tag_multiline_open(indent, name, attrs, self_close=False):
    pad = "\t" * indent
    lines = [f"{pad}<{name}"]
    for k, v in attrs:
        lines.append(f'{pad}\t{k}="{_esc(v)}"')
    lines[-1] += " />" if self_close else ">"
    return "\n".join(lines)


# ------------------------------------------------------------------------ sample data


class SampleRange:
    """One sample (or one range of a multisample / velocity-layered osc).

    file_name   path relative to the SD card ROOT ("SAMPLES/Foo/bar.wav"), even when the
                preset lives in a subfolder of SYNTHS/ or KITS/.
    top         rangeTopNote (or rangeTopVelocity when the osc is keyed by velocity).
                None on the LAST range: it defaults to 32767 (MultiRange) and catches the
                top of the keyboard / velocity 127. Sound::writeSourceToFile never writes
                the bound for the last range.
    transpose   60 - rootMidiNote (sample_holder_for_voice.cpp). Read back verbatim and
                never re-derived (sound.cpp readSourceFromFile), so always set it for
                pitched multisamples. Leave 0 for drums (natural rate).
    cents       fine tune, same rules.
    start/end   zone startSamplePos / endSamplePos; end 0 = to the end of the sample
                (sample_holder.cpp).
    level_db    optional measured level of the sample, used by validate() to check that a
                velocity ladder ascends. If absent, validate() measures the staged WAV.
    """

    def __init__(
        self,
        file_name,
        top=None,
        transpose=0,
        cents=0,
        start=0,
        end=0,
        loop_start=None,
        loop_end=None,
        level_db=None,
    ):
        self.file_name = file_name
        self.top = top
        self.transpose = int(transpose)
        self.cents = int(cents)
        self.start = int(start)
        self.end = int(end)
        self.loop_start = loop_start
        self.loop_end = loop_end
        self.level_db = level_db

    def __repr__(self):
        return f"SampleRange({self.file_name!r}, top={self.top!r}, transpose={self.transpose!r})"

    @classmethod
    def coerce(cls, r):
        if isinstance(r, SampleRange):
            return r
        if isinstance(r, str):
            return cls(r)
        if isinstance(r, dict):
            return cls(**r)
        if isinstance(r, (tuple, list)):
            return cls(*r)
        raise PresetError(f"cannot make a SampleRange from {r!r}")

    def zone_xml(self, indent):
        attrs = [("startSamplePos", self.start), ("endSamplePos", self.end)]
        if self.loop_start:
            attrs.append(("startLoopPos", self.loop_start))
        if self.loop_end:
            attrs.append(("endLoopPos", self.loop_end))
        return _tag_inline(indent, "zone", attrs)


def sample_ranges(ranges, keyed="note"):
    """Normalise + check a list of ranges for one oscillator.

    Enforces the two rules the reader has (sound.cpp readSourceFromFile):
      * bounds must be unique - a duplicate topNote is rejected as FILE_CORRUPTED;
      * the LAST range must omit its bound (it becomes 32767). Given a bounded last range
        the sample above it would have nowhere to go, so that is refused here too.
    Ranges are returned sorted by bound, last (unbounded) one at the end.
    """
    if keyed not in ("note", "velocity"):
        raise PresetError("keyed must be 'note' or 'velocity'")
    rs = [SampleRange.coerce(r) for r in ranges]
    if not rs:
        raise PresetError("sample_ranges() needs at least one range")
    unbounded = [r for r in rs if r.top is None]
    if len(unbounded) != 1:
        raise PresetError(
            f"exactly one range (the last) must omit its top bound; got {len(unbounded)} unbounded of {len(rs)}"
        )
    bounded = [r for r in rs if r.top is not None]
    tops = [int(r.top) for r in bounded]
    if len(set(tops)) != len(tops):
        dup = sorted({t for t in tops if tops.count(t) > 1})
        raise PresetError(
            f"duplicate range top {dup} - the firmware rejects that as FILE_CORRUPTED"
        )
    for t in tops:
        if not 0 <= t <= 127:
            raise PresetError(f"range top {t} is outside 0..127")
    bounded.sort(key=lambda r: int(r.top))
    return bounded + unbounded


# ------------------------------------------------------------------------ oscillators


class Osc:
    """<osc1> / <osc2> (Sound::writeSourceToFile).

    type: one of OSC_TYPES. For "sample" use set_sample() / set_ranges(); for "drum" pass
    drum_model (FORK ONLY - stock firmware plays a triangle instead).
    """

    def __init__(
        self,
        type="square",
        transpose=0,
        cents=0,
        retrig_phase=-1,
        drum_model=None,
        loop_mode=LOOP_MODE_CUT,
        reversed=0,
        time_stretch_enable=0,
        time_stretch_amount=0,
        linear_interpolation=False,
        oscillator_sync=False,
    ):
        if type not in OSC_TYPES:
            raise PresetError(f"unknown osc type {type!r}")
        if drum_model is not None and drum_model not in DRUM_MODELS:
            raise PresetError(
                "unknown drumModel {!r} (want one of {})".format(
                    drum_model, ", ".join(DRUM_MODELS)
                )
            )
        self.type = type
        self.transpose = int(transpose)
        self.cents = int(cents)
        self.retrig_phase = int(retrig_phase)
        self.drum_model = drum_model
        self.loop_mode = int(loop_mode)
        self.reversed = int(reversed)
        self.time_stretch_enable = int(time_stretch_enable)
        self.time_stretch_amount = int(time_stretch_amount)
        self.linear_interpolation = bool(linear_interpolation)
        self.oscillator_sync = bool(oscillator_sync)
        self.ranges = []
        self.keyed = "note"

    def set_sample(
        self, file_name, transpose=0, cents=0, start=0, end=0, loop_mode=None
    ):
        """One sample: fileName + zone go straight on the osc, no <sampleRanges> wrapper."""
        self.type = "sample"
        if loop_mode is not None:
            self.loop_mode = int(loop_mode)
        self.ranges = [SampleRange(file_name, None, transpose, cents, start, end)]
        return self

    def set_ranges(self, ranges, keyed="note", loop_mode=None):
        """Multisample (keyed="note") or velocity layers (keyed="velocity", FORK ONLY)."""
        self.type = "sample"
        if loop_mode is not None:
            self.loop_mode = int(loop_mode)
        self.ranges = sample_ranges(ranges, keyed)
        self.keyed = keyed
        return self

    def to_xml(self, tag, indent, fm=False):
        pad = "\t" * indent
        attrs = []
        if not fm:
            attrs.append(("type", self.type))
        if self.type == "sample" and not fm:
            attrs += [
                ("loopMode", self.loop_mode),
                ("reversed", self.reversed),
                ("timeStretchEnable", self.time_stretch_enable),
                ("timeStretchAmount", self.time_stretch_amount),
            ]
            if self.linear_interpolation:
                attrs.append(("linearInterpolation", 1))
            n = len(self.ranges)
            top_attr = (
                "rangeTopVelocity" if self.keyed == "velocity" else "rangeTopNote"
            )
            lines = []
            if n > 1:
                lines.append(_tag_multiline_open(indent, tag, attrs))
                lines.append(f"{pad}\t<sampleRanges>")
                for i, r in enumerate(self.ranges):
                    rattrs = []
                    if i != n - 1:
                        rattrs.append((top_attr, r.top))
                    rattrs.append(("fileName", r.file_name))
                    if r.transpose:
                        rattrs.append(("transpose", r.transpose))
                    if r.cents:
                        rattrs.append(("cents", r.cents))
                    lines.append(_tag_multiline_open(indent + 2, "sampleRange", rattrs))
                    lines.append(r.zone_xml(indent + 3))
                    lines.append(f"{pad}\t\t</sampleRange>")
                lines.append(f"{pad}\t</sampleRanges>")
                lines.append(f"{pad}</{tag}>")
            elif n == 1:
                r = self.ranges[0]
                attrs.append(("fileName", r.file_name))
                if r.transpose:
                    attrs.append(("transpose", r.transpose))
                if r.cents:
                    attrs.append(("cents", r.cents))
                lines.append(_tag_multiline_open(indent, tag, attrs))
                lines.append(r.zone_xml(indent + 1))
                lines.append(f"{pad}</{tag}>")
            else:
                lines.append(_tag_multiline_open(indent, tag, attrs))
                lines.append(f"{pad}</{tag}>")
            return "\n".join(lines)
        attrs += [("transpose", self.transpose), ("cents", self.cents)]
        if tag == "osc2" and self.oscillator_sync:
            attrs.append(("oscillatorSync", 1))
        attrs.append(("retrigPhase", self.retrig_phase))
        if self.type == "drum" and self.drum_model:
            attrs.append(("drumModel", self.drum_model))
        return _tag_multiline_open(indent, tag, attrs, self_close=True)


# ------------------------------------------------------------------------------ Sound


class Sound:
    """A <sound>: a SYNTH preset on its own, or one drum row inside a Kit.

    Attribute / element order is Sound::writeToFile (processing/sound/sound.cpp):
      name (drum rows only) polyphonic voicePriority [sideChainSend] mode [transpose]
      modFXType lpfMode hpfMode filterRoute [clippingAmount] [path] maxVoices
      <osc1> <osc2> <lfo1..4> [<modulator1/2> if FM] <unison> <defaultParams>
      (<modKnobs>) <delay> <sidechain>
    <arpeggiator>, <midiOutput>, <audioCompressor>, <stutter> and <midiKnobs> are
    optional for the reader and left out unless set.
    """

    def __init__(
        self,
        name=None,
        polyphonic="poly",
        voice_priority=1,
        mode="subtractive",
        side_chain_send=None,
        transpose=0,
        mod_fx_type="none",
        lpf_mode="24dB",
        hpf_mode="HPLadder",
        filter_route="H2L",
        max_voices=8,
        path=None,
        clipping_amount=None,
    ):
        if polyphonic not in POLYPHONY_MODES:
            raise PresetError(
                "polyphonic must be one of {}".format(", ".join(POLYPHONY_MODES))
            )
        if mode not in SYNTH_MODES:
            raise PresetError("mode must be one of {}".format(", ".join(SYNTH_MODES)))
        if mod_fx_type not in MOD_FX_TYPES:
            raise PresetError(
                "modFXType must be one of {}".format(", ".join(MOD_FX_TYPES))
            )
        if lpf_mode not in FILTER_MODES or hpf_mode not in FILTER_MODES:
            raise PresetError(
                "lpfMode/hpfMode must be one of {}".format(", ".join(FILTER_MODES))
            )
        if filter_route not in FILTER_ROUTES:
            raise PresetError(
                "filterRoute must be one of {}".format(", ".join(FILTER_ROUTES))
            )
        self.name = name
        self.polyphonic = polyphonic
        self.voice_priority = int(voice_priority)
        self.side_chain_send = side_chain_send
        self.mode = mode
        self.transpose = int(transpose)
        self.mod_fx_type = mod_fx_type
        self.lpf_mode = lpf_mode
        self.hpf_mode = hpf_mode
        self.filter_route = filter_route
        self.clipping_amount = clipping_amount
        self.path = path
        self.max_voices = int(max_voices)
        self.osc1 = Osc("square")
        self.osc2 = Osc("square")
        self.lfos = [
            ["triangle", 0, 0],
            ["triangle", 0, 0],
            ["triangle", 0, 0],
            ["triangle", 0, 0],
        ]
        self.modulators = [
            {"transpose": 0, "cents": 0, "retrigPhase": -1},
            {"transpose": 0, "cents": 0, "retrigPhase": -1, "toModulator1": 0},
        ]
        self.unison = {"num": 1, "detune": 8, "spread": 0}
        self.params = dict(SOUND_PARAM_DEFAULTS)
        self.optional_params = {}
        self.envelopes = [dict(e) for e in ENVELOPE_DEFAULTS]
        self.cables = []
        self.equalizer = dict(EQUALIZER_DEFAULTS)
        self.mod_knobs = list(DEFAULT_MOD_KNOBS)
        self.write_mod_knobs = True
        self.delay = {"pingPong": 1, "analog": 0, "syncLevel": 7, "syncType": 0}
        self.sidechain = {
            "attack": 327244,
            "release": 936,
            "syncLevel": 6,
            "syncType": 0,
        }
        self.arpeggiator = None  # dict of attributes, written verbatim if set

    # ---- configuration helpers
    def set(self, **params):
        """Set <defaultParams> attributes by name; values are hex strings (or ints -> hex32).
        Names must be ones Sound::writeParamsToFile knows."""
        for k, v in params.items():
            if not isinstance(v, str):
                v = hex32(v)
            _check_hex(v, f"param {k}")
            if k in self.params:
                self.params[k] = v
            elif k in SOUND_OPTIONAL_PARAMS:
                self.optional_params[k] = v
            else:
                raise PresetError(f"unknown defaultParams attribute {k!r}")
        return self

    def envelope(self, n, attack=None, decay=None, sustain=None, release=None):
        """Envelope 1..4. Values: hex string, or int 0..50 knob value (-> knob())."""
        env = self.envelopes[n - 1]
        for k, v in (
            ("attack", attack),
            ("decay", decay),
            ("sustain", sustain),
            ("release", release),
        ):
            if v is None:
                continue
            if isinstance(v, int):
                v = knob(v)
            _check_hex(v, f"envelope{n} {k}")
            env[k] = v
        return self

    def lfo(self, n, type="triangle", sync_level=0, sync_type=0):
        if type not in LFO_TYPES:
            raise PresetError(f"unknown LFO type {type!r}")
        self.lfos[n - 1] = [type, int(sync_level), int(sync_type)]
        return self

    def cable(self, source, destination, amount, polarity="bipolar"):
        self.cables.append(PatchCable(source, destination, amount, polarity))
        return self

    def eq(self, **kv):
        for k, v in kv.items():
            if k not in self.equalizer:
                raise PresetError(f"unknown equalizer attribute {k!r}")
            _check_hex(v, f"equalizer {k}")
            self.equalizer[k] = v
        return self

    # ---- preset lookups for the simulator
    def preset_for(self, destination):
        """The preset value the patcher folds in for a cable destination."""
        attr = DEST_PRESET_ATTR.get(destination, destination)
        if attr is None:
            return 0
        if isinstance(attr, tuple):
            return self.envelopes[int(attr[0][-1]) - 1][attr[1]]
        if attr in self.params:
            return self.params[attr]
        return self.optional_params.get(attr, "0x00000000")

    def sources(self):
        return [self.osc1, self.osc2]

    def sample_files(self):
        return [
            r.file_name for o in self.sources() if o.type == "sample" for r in o.ranges
        ]

    # ---- serialisation
    def _root_attrs(self, as_drum):
        attrs = []
        if not as_drum:
            attrs += [
                ("firmwareVersion", FIRMWARE_VERSION),
                ("earliestCompatibleFirmware", EARLIEST_COMPATIBLE_FIRMWARE),
            ]
        if as_drum:
            attrs.append(("name", self.name or ""))
        elif self.name:
            attrs.append(("name", self.name))
        attrs += [
            ("polyphonic", self.polyphonic),
            ("voicePriority", self.voice_priority),
        ]
        if self.side_chain_send:
            attrs.append(("sideChainSend", self.side_chain_send))
        attrs.append(("mode", self.mode))
        if self.transpose:
            attrs.append(("transpose", self.transpose))
        attrs += [
            ("modFXType", self.mod_fx_type),
            ("lpfMode", self.lpf_mode),
            ("hpfMode", self.hpf_mode),
            ("filterRoute", self.filter_route),
        ]
        if self.clipping_amount:
            attrs.append(("clippingAmount", self.clipping_amount))
        if as_drum and self.path:
            attrs.append(("path", self.path))
        attrs.append(("maxVoices", self.max_voices))
        return attrs

    def _default_params_xml(self, indent):
        pad = "\t" * indent
        attrs = []
        for k, v in self.params.items():
            attrs.append((k, v))
            if k == "carrier2Feedback":
                for opt in SOUND_OPTIONAL_PARAMS:
                    if opt in self.optional_params and opt != "tapeSaturation":
                        attrs.append((opt, self.optional_params[opt]))
            if k == "modFXFeedback" and "tapeSaturation" in self.optional_params:
                attrs.append(("tapeSaturation", self.optional_params["tapeSaturation"]))
        lines = [_tag_multiline_open(indent, "defaultParams", attrs)]
        for i, env in enumerate(self.envelopes, start=1):
            lines.append(
                _tag_multiline_open(
                    indent + 1,
                    f"envelope{i}",
                    [(f, env[f]) for f in ENVELOPE_FIELDS],
                    self_close=True,
                )
            )
        if self.cables:
            lines.append(f"{pad}\t<patchCables>")
            for c in self.cables:
                lines.append(c.to_xml(indent + 2))
            lines.append(f"{pad}\t</patchCables>")
        lines.append(
            _tag_multiline_open(
                indent + 1, "equalizer", list(self.equalizer.items()), self_close=True
            )
        )
        lines.append(f"{pad}</defaultParams>")
        return "\n".join(lines)

    def to_xml(self, as_drum=False, indent=0):
        pad = "\t" * indent
        fm = self.mode == "fm"
        lines = [_tag_multiline_open(indent, "sound", self._root_attrs(as_drum))]
        lines.append(self.osc1.to_xml("osc1", indent + 1, fm))
        lines.append(self.osc2.to_xml("osc2", indent + 1, fm))
        for i, (t, sl, st) in enumerate(self.lfos, start=1):
            lines.append(
                _tag_inline(
                    indent + 1,
                    f"lfo{i}",
                    [("type", t), ("syncLevel", sl), ("syncType", st)],
                )
            )
        if fm:
            for i, m in enumerate(self.modulators, start=1):
                lines.append(_tag_inline(indent + 1, f"modulator{i}", list(m.items())))
        lines.append(_tag_inline(indent + 1, "unison", list(self.unison.items())))
        lines.append(self._default_params_xml(indent + 1))
        if self.arpeggiator:
            lines.append(
                _tag_multiline_open(
                    indent + 1,
                    "arpeggiator",
                    list(self.arpeggiator.items()),
                    self_close=True,
                )
            )
        if self.write_mod_knobs:
            lines.append(f"{pad}\t<modKnobs>")
            for param, src in self.mod_knobs:
                attrs = [("controlsParam", param)]
                if src:
                    attrs.append(("patchAmountFromSource", src))
                lines.append(_tag_inline(indent + 2, "modKnob", attrs))
            lines.append(f"{pad}\t</modKnobs>")
        lines.append(_tag_inline(indent + 1, "delay", list(self.delay.items())))
        lines.append(_tag_inline(indent + 1, "sidechain", list(self.sidechain.items())))
        lines.append(f"{pad}</sound>")
        return "\n".join(lines)

    def xml_document(self):
        return '<?xml version="1.0" encoding="UTF-8"?>\n' + self.to_xml() + "\n"

    def write(self, path):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(self.xml_document())
        return path

    # ---- validation
    def validate(self, staging_root=None, label=None, check_ladders=True):
        """-> Report. Errors are things the firmware rejects or that can never work;
        warnings are audible-but-legal (a cable that silences an osc at some velocity)."""
        rep = Report(label or self.name or "sound")
        for k, v in list(self.params.items()) + list(self.optional_params.items()):
            _validate_param_hex(rep, k, v)
        for i, env in enumerate(self.envelopes, start=1):
            for f in ENVELOPE_FIELDS:
                _validate_param_hex(rep, f"envelope{i}.{f}", env[f])
        for k, v in self.equalizer.items():
            _validate_param_hex(rep, f"equalizer.{k}", v)
        if self.osc1.type == "drum" or self.osc2.type == "drum":
            rep.note(
                'uses type="drum" (FORK ONLY: stock firmware plays a triangle wave instead)'
            )
        for tag, osc in (("osc1", self.osc1), ("osc2", self.osc2)):
            if osc.type != "sample":
                continue
            if not osc.ranges:
                rep.warn(f"{tag} is type sample but has no file")
            if osc.keyed == "velocity":
                rep.note(
                    f"{tag} uses rangeTopVelocity (FORK ONLY: stock firmware fails with FILE_CORRUPTED / "
                    '"SD card error"; the fork needs the Drum Velocity Layers feature ON)'
                )
            try:
                sample_ranges(osc.ranges, osc.keyed)
            except PresetError as e:
                rep.error(f"{tag}: {e}")
            _validate_sample_files(rep, tag, osc, staging_root, check_ladders)
        _validate_cables(rep, self)
        return rep


def _validate_param_hex(rep, name, value):
    if not isinstance(value, str) or not _HEX_RE.match(value):
        rep.error(f"{name}: {value!r} is not an 8-digit 0x hex value")
        return
    if name in UNIPOLAR_PARAMS and int(value, 16) > 0x7FFFFFFF:
        rep.error(
            f"{name}: {value} is outside the unipolar range 0x00000000..0x7FFFFFFF"
        )


def _validate_sample_files(rep, tag, osc, staging_root, check_ladders):
    levels = []
    for r in osc.ranges:
        ext = os.path.splitext(r.file_name)[1].lower()
        if ext not in (".wav", ".aif", ".aiff"):
            rep.error(
                f"{tag}: {r.file_name} - the Deluge only reads WAV/AIFF (no FLAC/MP3); convert to 44.1 kHz 16-bit WAV"
            )
        if not r.file_name.startswith("SAMPLES/"):
            rep.warn(
                f"{tag}: {r.file_name} does not start with SAMPLES/ - paths are relative to the SD card root"
            )
        if staging_root is None:
            levels.append(r.level_db)
            continue
        p = os.path.join(staging_root, r.file_name)
        if not os.path.isfile(p):
            rep.error(f"{tag}: missing sample {r.file_name} (looked in {staging_root})")
            levels.append(r.level_db)
            continue
        info = wav_info(p)
        if info:
            if info["rate"] != SAMPLE_RATE:
                rep.warn(
                    f"{tag}: {r.file_name} is {info['rate']} Hz; 44100 plays 1:1 without resampling"
                )
            if info["bits"] != 16:
                rep.warn(
                    f"{tag}: {r.file_name} is {info['bits']}-bit; 16-bit is the safe format"
                )
        lvl = r.level_db
        if (
            lvl is None
            and check_ladders
            and osc.keyed == "velocity"
            and len(osc.ranges) > 1
        ):
            try:
                lvl = wav_peak_dbfs(p)
            except (
                OSError,
                ValueError,
                wave.Error,
                EOFError,
            ) as e:  # unreadable -> no ladder check
                rep.warn(f"{tag}: could not measure {r.file_name} ({e})")
        levels.append(lvl)
    if (
        check_ladders
        and osc.keyed == "velocity"
        and len(osc.ranges) > 1
        and all(l is not None for l in levels)
        and any(b <= a for a, b in itertools.pairwise(levels))
    ):
        ladder = ", ".join(f"{l:.1f}" for l in levels)
        rep.error(
            f"{tag}: velocity ladder does not ascend: {ladder} dB - a louder sample in a "
            "lower band makes a drum that gets quieter when hit harder"
        )
    if osc.keyed == "note" and len(osc.ranges) > 1:
        roots = [60 - r.transpose for r in osc.ranges]
        if any(b <= a for a, b in itertools.pairwise(roots)):
            rep.warn(
                f"{tag}: root notes (60 - transpose) are not ascending with the ranges: {roots}"
            )


def _validate_cables(rep, sound):
    by_dest = {}
    for c in sound.cables:
        by_dest.setdefault(c.destination, []).append(c)
    for dest, cables in by_dest.items():
        kind = DEST_KIND[dest]
        preset = sound.preset_for(dest)
        if kind in ("volume", "linear"):
            if s32(preset) == INT32_MIN:
                rep.error(
                    f"{dest} is parked at 0x80000000 with a cable on it: the linear patcher multiplies, so "
                    "the running total is zero before any cable applies - it is silent forever and "
                    "cannot be patched back up"
                )
                continue
            for c in cables:
                if abs(c.amount_int) > 0x40000000 and kind == "volume":
                    rep.warn(
                        f"{dest}: cable {c.source} amount {c.amount} exceeds +/-0x40000000; a full-scale cable drives the "
                        "factor through zero mid-sweep"
                    )
            if any(c.source == "velocity" for c in cables):
                silent = Patcher.silent_velocities(dest, preset, cables)
                if silent and kind == "volume":
                    # split the near-silent (<= peak/100, i.e. -40 dB) velocities into runs
                    runs, run = [], [silent[0]]
                    for v in silent[1:]:
                        if v == run[-1] + 1:
                            run.append(v)
                        else:
                            runs.append(run)
                            run = [v]
                    runs.append(run)
                    for run in runs:
                        lo, hi = run[0], run[-1]
                        if lo == 1 and hi < 127:
                            # the ordinary soft end of a velocity-sensitive sound
                            rep.note(
                                f"{dest} fades below -40 dB at velocity <= {hi} (cables {cables})"
                            )
                        elif hi == 127 and lo > 1:
                            rep.warn(
                                f"{dest} is silenced at velocity >= {lo} (cables {cables})"
                            )
                        else:
                            rep.warn(
                                f"{dest} is driven through zero at velocities {lo}..{hi} - the linear factor "
                                f"crosses zero mid-sweep and flips sign beyond (cables {cables})"
                            )
            for c in cables:
                if c.source in ("velocity", "note"):
                    continue
                # sweep the other sources across their range with velocity at 64
                vals = [
                    Patcher.evaluate(dest, preset, cables, {c.source: x / 16.0})
                    for x in range(-16, 17)
                ]
                peak = max(vals)
                if peak > 0 and min(vals) <= peak // 100 and kind == "volume":
                    rep.warn(
                        f"{dest}: cable from {c.source} ({c.amount}) nulls the parameter at one end of its range"
                    )


# -------------------------------------------------------------------------------- Kit


class Kit:
    """A <kit>: kit-level effects/params + a <soundSources> array of Sound rows.

    Rows appear on the grid bottom-up in file order (first row = bottom pad). Kit::writeToFile
    (model/instrument/kit.cpp): attributes from GlobalEffectable::writeAttributesToFile +
    ModControllableAudio::writeAttributesToFile, then <defaultParams>, <delay>, <sidechain>,
    <soundSources>, <selectedDrumIndex>.
    """

    def __init__(
        self,
        mod_fx_type="none",
        lpf_mode="24dB",
        hpf_mode="HPLadder",
        filter_route="H2L",
        mod_fx_current_param="feedback",
        current_filter_type="lpf",
        selected_drum_index=0,
    ):
        self.mod_fx_type = mod_fx_type
        self.lpf_mode = lpf_mode
        self.hpf_mode = hpf_mode
        self.filter_route = filter_route
        self.mod_fx_current_param = mod_fx_current_param
        self.current_filter_type = current_filter_type
        self.selected_drum_index = selected_drum_index
        self.params = dict(KIT_PARAM_DEFAULTS)
        self.param_tags = {tag: dict(attrs) for tag, attrs in KIT_PARAM_TAGS}
        self.delay = {"pingPong": 1, "analog": 0, "syncLevel": 7, "syncType": 0}
        self.sidechain = {
            "attack": 327244,
            "release": 936,
            "syncLevel": 6,
            "syncType": 0,
        }
        self.rows = []

    def set(self, **params):
        for k, v in params.items():
            if not isinstance(v, str):
                v = hex32(v)
            _check_hex(v, f"kit param {k}")
            if k not in self.params:
                raise PresetError(f"unknown kit defaultParams attribute {k!r}")
            self.params[k] = v
        return self

    def add(self, sound):
        """Append a drum row (a Sound with a name). Returns the Sound."""
        if not isinstance(sound, Sound):
            raise PresetError("Kit.add() wants a Sound")
        if not sound.name:
            raise PresetError("kit rows need a name")
        self.rows.append(sound)
        return sound

    def drum(self, name, **kw):
        """Create + add a row with sample-drum defaults (Sound::setupAsSample):
        polyphonic auto, env1 A0 D20 S50 R0, velocity->volume at knob 50 (0x3FFFFFE8)."""
        s = Sound(name=name, polyphonic=kw.pop("polyphonic", "auto"), **kw)
        s.envelope(1, attack=0, decay=20, sustain=50, release=0)
        s.cable("velocity", "volume", knob(50, "cable"))
        return self.add(s)

    def to_xml(self):
        attrs = [
            ("firmwareVersion", FIRMWARE_VERSION),
            ("earliestCompatibleFirmware", EARLIEST_COMPATIBLE_FIRMWARE),
            ("modFXCurrentParam", self.mod_fx_current_param),
            ("currentFilterType", self.current_filter_type),
            ("modFXType", self.mod_fx_type),
            ("lpfMode", self.lpf_mode),
            ("hpfMode", self.hpf_mode),
            ("filterRoute", self.filter_route),
        ]
        lines = [_tag_multiline_open(0, "kit", attrs)]
        lines.append(_tag_multiline_open(1, "defaultParams", list(self.params.items())))
        for tag, _ in KIT_PARAM_TAGS:
            lines.append(_tag_inline(2, tag, list(self.param_tags[tag].items())))
        lines.append("\t</defaultParams>")
        lines.append(_tag_inline(1, "delay", list(self.delay.items())))
        lines.append(_tag_inline(1, "sidechain", list(self.sidechain.items())))
        lines.append("\t<soundSources>")
        for row in self.rows:
            lines.append(row.to_xml(as_drum=True, indent=2))
        lines.append("\t</soundSources>")
        if self.selected_drum_index is not None:
            lines.append(
                f"\t<selectedDrumIndex>{self.selected_drum_index}</selectedDrumIndex>"
            )
        lines.append("</kit>")
        return "\n".join(lines)

    def xml_document(self):
        return '<?xml version="1.0" encoding="UTF-8"?>\n' + self.to_xml() + "\n"

    def write(self, path):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(self.xml_document())
        return path

    def validate(self, staging_root=None, check_ladders=True):
        rep = Report("kit")
        if not self.rows:
            rep.error("kit has no rows")
        for k, v in self.params.items():
            _validate_param_hex(rep, f"kit.{k}", v)
        chokes = [r.name for r in self.rows if r.polyphonic == "choke"]
        if len(chokes) > 1:
            rep.note(
                "choke group: {} (there is ONE choke group per kit; each chokes all the others)".format(
                    ", ".join(chokes)
                )
            )
        names = [r.name for r in self.rows]
        if len(set(names)) != len(names):
            rep.warn(
                f"duplicate row names: {sorted({n for n in names if names.count(n) > 1})}"
            )
        for row in self.rows:
            rep.merge(
                row.validate(staging_root, label=row.name, check_ladders=check_ladders)
            )
        return rep


# ----------------------------------------------------------------------------- Report


class Report:
    def __init__(self, label):
        self.label = label
        self.errors = []
        self.warnings = []
        self.notes = []

    def error(self, msg):
        self.errors.append(f"{self.label}: {msg}")

    def warn(self, msg):
        self.warnings.append(f"{self.label}: {msg}")

    def note(self, msg):
        self.notes.append(f"{self.label}: {msg}")

    def merge(self, other):
        self.errors += other.errors
        self.warnings += other.warnings
        self.notes += other.notes
        return self

    @property
    def ok(self):
        return not self.errors

    def __str__(self):
        out = []
        for e in self.errors:
            out.append("ERROR   " + e)
        for w in self.warnings:
            out.append("WARNING " + w)
        for n in self.notes:
            out.append("note    " + n)
        return "\n".join(out) if out else f"{self.label}: ok"

    def raise_on_error(self):
        if self.errors:
            raise PresetError("\n".join(self.errors))
        return self


# --------------------------------------------------------------------------- WAV utils


def wav_info(path):
    """{'rate','bits','channels','frames'} for a WAV, or None if the wave module can't read it."""
    try:
        with wave.open(path, "rb") as w:
            return {
                "rate": w.getframerate(),
                "bits": w.getsampwidth() * 8,
                "channels": w.getnchannels(),
                "frames": w.getnframes(),
            }
    except (wave.Error, EOFError, OSError):
        return None


def wav_peak_dbfs(path):
    """Peak level of a 16- or 24-bit PCM WAV in dBFS (stdlib only; audioop is gone in 3.13)."""
    with wave.open(path, "rb") as w:
        width = w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if width == 2:
        a = array.array("h")
        a.frombytes(raw[: len(raw) - len(raw) % 2])
        peak = max(abs(v) for v in a) if len(a) else 0
        return 20 * math.log10(peak / 32768.0) if peak else -math.inf
    if width == 3:
        n = len(raw) // 3
        peak = 0
        for i in range(0, n * 3, 3):
            v = int.from_bytes(raw[i : i + 3], "little", signed=True)
            peak = max(peak, abs(v))
        return 20 * math.log10(peak / 8388608.0) if peak else -math.inf
    raise ValueError(f"unsupported sample width {width} bytes")


def _iter_chunks(buf):
    pos = 12
    while pos + 8 <= len(buf):
        cid = buf[pos : pos + 4]
        size = struct.unpack_from("<I", buf, pos + 4)[0]
        yield pos, cid, size
        pos += 8 + size + (size & 1)


def wav_unity_note(path):
    """dwMIDIUnityNote from the WAV's smpl chunk, or None. The firmware's chunk scanner
    (audio_file.cpp) runs to EOF, so a smpl appended after data is found too."""
    with open(path, "rb") as fh:
        buf = fh.read()
    if buf[:4] != b"RIFF" or buf[8:12] != b"WAVE":
        return None
    for pos, cid, size in _iter_chunks(buf):
        if cid == b"smpl" and size >= 36:
            return struct.unpack_from("<I", buf, pos + 8 + 12)[0]
    return None


def stamp_root_note(path, midi):
    """Write dwMIDIUnityNote into the WAV's smpl chunk, creating one if absent.

    Presets carry explicit transpose values so they do not need this, but it makes the
    on-device "load folder as multisample" path work. Beware: if EVERY file in a folder
    carries the SAME unity note the loader discards them all and pitch-detects instead.
    Returns 'patched' or 'added'.
    """
    with open(path, "rb") as fh:
        buf = fh.read()
    if buf[:4] != b"RIFF" or buf[8:12] != b"WAVE":
        raise ValueError("not RIFF/WAVE: " + path)
    for pos, cid, size in _iter_chunks(buf):
        if cid == b"smpl" and size >= 36:
            out = bytearray(buf)
            struct.pack_into("<I", out, pos + 8 + 12, midi)
            struct.pack_into("<I", out, pos + 8 + 16, 0)
            with open(path, "wb") as fh:
                fh.write(out)
            return "patched"
    body = struct.pack(
        "<IIIIIIIII", 0, 0, 1000000000 // SAMPLE_RATE, midi, 0, 0, 0, 0, 0
    )
    out = bytearray(buf)
    if len(out) & 1:
        out += b"\x00"
    out += b"smpl" + struct.pack("<I", len(body)) + body
    struct.pack_into("<I", out, 4, len(out) - 8)
    with open(path, "wb") as fh:
        fh.write(out)
    return "added"


def write_silent_wav(path, seconds=0.05, rate=SAMPLE_RATE, unity_note=None):
    """A tiny valid 16-bit mono WAV, for placeholder samples in examples/tests."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"\x00\x00" * int(seconds * rate))
    if unity_note is not None:
        stamp_root_note(path, unity_note)
    return path


def write_tone_wav(
    path, peak_dbfs=-6.0, seconds=0.05, freq=440.0, rate=SAMPLE_RATE, unity_note=None
):
    """A short sine at a chosen peak level - lets examples build a real, checkable velocity ladder."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    amp = 10 ** (peak_dbfs / 20.0) * 32767
    n = int(seconds * rate)
    frames = array.array(
        "h", (int(amp * math.sin(2 * math.pi * freq * i / rate)) for i in range(n))
    )
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(frames.tobytes())
    if unity_note is not None:
        stamp_root_note(path, unity_note)
    return path


# ------------------------------------------------------------------- reading back


def flatten_preset_xml(path_or_text):
    """{xpath-ish key: value} for a preset file, for diffing two files semantically.

    Handles both the attribute style the device writes now and the old
    <tag>value</tag> element style (contrib/plaits_drums/make_kits.py). Rows are keyed by
    index so order-sensitive things (soundSources, patchCables) still compare.
    """
    import xml.etree.ElementTree as ET

    if "<" in path_or_text:
        root = ET.fromstring(path_or_text)
    else:
        root = ET.parse(path_or_text).getroot()
    out = {}

    def walk(el, prefix):
        for k, v in el.attrib.items():
            out[prefix + "@" + k] = v
        text = (el.text or "").strip()
        if text and len(el) == 0:
            out[prefix] = text
        counts = {}
        for child in el:
            n = counts.get(child.tag, 0)
            counts[child.tag] = n + 1
            key = prefix + "/" + child.tag
            if child.tag in ("sound", "patchCable", "sampleRange", "modKnob"):
                key += f"[{n}]"
            walk(child, key)

    walk(root, root.tag)
    # element-style files put the leaf value in the element; attribute style in "@name":
    # normalise "a/b/c" == "a/b@c" for leaf values so the two styles diff cleanly.
    norm = {}
    for k, v in out.items():
        if "@" in k:
            base, attr = k.rsplit("@", 1)
            norm[base + "/" + attr] = v
        else:
            norm[k] = v
    return norm


__all__ = [n for n in dir() if not n.startswith("_")]
