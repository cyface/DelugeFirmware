#!/usr/bin/env python3
"""Unit tests for deluge_preset.py: python3 -m unittest test_deluge_preset -v"""

import os
import shutil
import tempfile
import unittest

import deluge_preset as dp


class Encoders(unittest.TestCase):
    def test_half_full_endpoints(self):
        self.assertEqual(dp.half(0), "0x00000000")
        self.assertEqual(dp.half(1), "0x7FFFFFFF")
        self.assertEqual(dp.full(0), "0x80000000")
        self.assertEqual(dp.full(1), "0x7FFFFFFF")
        self.assertEqual(
            dp.full(0.5), "0x00000000"
        )  # 0.5 * 0xFFFFFFFF rounds to 0x80000000 -> 0
        self.assertEqual(dp.half(2), "0x7FFFFFFF")  # clamped
        self.assertEqual(dp.full(-1), "0x80000000")

    def test_knob_matches_getParamFromUserValue(self):
        # default case: userValue * 85899345 - 2147483648 (util/functions.cpp)
        self.assertEqual(dp.knob(0), "0x80000000")
        self.assertEqual(dp.knob(20), "0xE6666654")  # envelope2 defaults on the device
        self.assertEqual(dp.knob(25), "0xFFFFFFE9")
        self.assertEqual(
            dp.knob(50), "0x7FFFFFD2"
        )  # what the device writes for 50, not 0x7FFFFFFF
        self.assertEqual(dp.knob(6), "0x9EB851E6")
        # PATCH_CABLE: userValue * 21474836
        self.assertEqual(dp.knob(50, "cable"), "0x3FFFFFE8")
        self.assertEqual(dp.knob(-50, "cable"), dp.hex32(-50 * 21474836))
        # phase width: unsigned userValue * (85899345 >> 1)
        self.assertEqual(dp.knob(50, "pulseWidth"), dp.hex32(50 * (85899345 >> 1)))
        # eq
        self.assertEqual(dp.knob(-50, "eq"), "0x80000000")
        self.assertEqual(dp.knob(0, "eq"), "0x00000000")
        with self.assertRaises(ValueError):
            dp.knob(51)
        with self.assertRaises(ValueError):
            dp.knob(-1)

    def test_db_is_square_law(self):
        self.assertEqual(dp.db(0), "0x7FFFFFFF")
        # the centre preset (0x00000000) is -12 dB because the volume law squares
        self.assertAlmostEqual(dp.db_of("0x00000000"), -12.04, places=2)
        self.assertAlmostEqual(dp.db_of(dp.db(-20)), -20.0, places=1)
        self.assertAlmostEqual(dp.db_of(dp.db(-3)), -3.0, places=1)
        with self.assertRaises(ValueError):
            dp.db(3)

    def test_semitones_and_cents(self):
        self.assertEqual(dp.semitones(12), "0x20000000")  # one octave of preset = 2^29
        self.assertEqual(dp.semitones(-12), "0xE0000000")
        self.assertEqual(dp.semitones(48), "0x7FFFFFFF")  # saturates at 4 octaves
        self.assertEqual(dp.cents(1200), dp.semitones(12))

    def test_hex_helpers(self):
        self.assertEqual(dp.s32("0x80000000"), -2147483648)
        self.assertEqual(dp.s32(0x7FFFFFFF), 2147483647)
        self.assertEqual(dp.hex32(-1), "0xFFFFFFFF")


class EnvelopeTiming(unittest.TestCase):
    def test_release_table_matches_documented_values(self):
        # knob 6 ~ 0.36 s, 20 ~ 1.64 s, 30 ~ 3.46 s, 50 ~ 27 s (Salamander notes, verified)
        self.assertAlmostEqual(dp.release_seconds(6), 0.36, places=2)
        self.assertAlmostEqual(dp.release_seconds(20), 1.64, places=2)
        self.assertAlmostEqual(dp.release_seconds(30), 3.46, places=2)
        self.assertAlmostEqual(dp.release_seconds(50), 27.17, places=1)
        # decay uses half the neutral rate -> twice the time
        self.assertAlmostEqual(
            dp.decay_seconds(20) / dp.release_seconds(20), 2.0, places=3
        )
        # a hex preset works too
        self.assertAlmostEqual(dp.release_seconds("0xE6666654"), 1.64, places=2)

    def test_lookup_release_rate_hand_values(self):
        # patched = knob-20 preset * 2^30 >> 32 = -107374187 -> index 25, interpolated
        patched = (dp.s32("0xE6666654") * 1073741824) >> 32
        self.assertEqual(patched, -107374187)
        rate = dp.lookup_release_rate(patched)
        self.assertTrue(
            dp.RELEASE_RATE_TABLE_64[26] <= rate <= dp.RELEASE_RATE_TABLE_64[25]
        )
        self.assertEqual(
            dp.lookup_release_rate(-(1 << 31)), dp.RELEASE_RATE_TABLE_64[0]
        )
        self.assertEqual(
            dp.lookup_release_rate((1 << 31) - 1), dp.RELEASE_RATE_TABLE_64[64]
        )

    def test_attack_zero_is_instant(self):
        self.assertEqual(dp.attack_seconds(0), 0.0)
        self.assertGreater(dp.attack_seconds(50), 1.0)


class PatcherSim(unittest.TestCase):
    """Hand-computed values through cableToLinearParam / getFinalParameterValueVolume."""

    def test_source_values(self):
        self.assertEqual(dp.Patcher.source_value("velocity", 64), 0)
        self.assertEqual(dp.Patcher.source_value("velocity", 0), -64 * 33554432)
        self.assertEqual(dp.Patcher.source_value("velocity", 127), 63 * 33554432)
        self.assertEqual(dp.Patcher.source_value("velocity", 128), dp.INT32_MAX)

    def test_preset_only_volume_law(self):
        # no cables: preset max -> "4" * neutral = full scale; preset centre -> neutral (1/4 = -12 dB)
        full_scale = 4 * dp.param_neutral("oscAVolume")
        self.assertAlmostEqual(
            dp.Patcher.evaluate("oscAVolume", "0x7FFFFFFF", [], {}) / full_scale,
            1.0,
            places=3,
        )
        self.assertAlmostEqual(
            dp.Patcher.gain_db("oscAVolume", "0x7FFFFFFF", [], {}), 0.0, places=2
        )
        self.assertEqual(
            dp.Patcher.evaluate("oscAVolume", "0x00000000", [], {}),
            dp.param_neutral("oscAVolume"),
        )
        self.assertEqual(dp.Patcher.evaluate("oscAVolume", "0x80000000", [], {}), 0)
        self.assertAlmostEqual(
            dp.Patcher.gain_db("oscAVolume", "0x00000000", [], {}), -12.04, places=2
        )

    def test_cable_to_linear_hand_values(self):
        # running_total "1", source at +max, strength 0x40000000:
        #   scaled = INT32_MAX * 2^30 >> 32 = 536870911; made_positive = 1073741823
        #   product = 2^29 * 1073741823 >> 32 = 134217727; << 3 = 1073741816  ("2")
        rt = dp.Patcher.cable_to_linear(dp.ONE, dp.INT32_MAX, 0x40000000)
        self.assertEqual(rt, 1073741816)
        # source at -max: made_positive ~ 0 -> product 0 (the multiplicative null)
        rt = dp.Patcher.cable_to_linear(dp.ONE, dp.INT32_MIN, 0x40000000)
        self.assertEqual(rt, 0)
        # preset 0x80000000 as "cable" with range 2^30: exactly -536870912 -> zero for ever
        self.assertEqual(
            dp.Patcher.cable_to_linear(dp.ONE, dp.INT32_MIN, 1073741824), 0
        )

    def test_centred_crossfade(self):
        cables = [
            dp.PatchCable("velocity", "oscAVolume", (-0x40000000) & 0xFFFFFFFF),
            dp.PatchCable("velocity", "oscBVolume", 0x40000000),
        ]
        a64 = dp.Patcher.gain_db("oscAVolume", "0x00000000", cables, {"velocity": 64})
        b64 = dp.Patcher.gain_db("oscBVolume", "0x00000000", cables, {"velocity": 64})
        self.assertAlmostEqual(
            a64, -12.04, places=2
        )  # each layer at 1/4 -> the ~6 dB dip of the sum
        self.assertAlmostEqual(b64, -12.04, places=2)
        self.assertAlmostEqual(
            dp.Patcher.gain_db("oscBVolume", "0x00000000", cables, {"velocity": 127}),
            -0.14,
            places=2,
        )
        self.assertLess(
            dp.Patcher.gain_db("oscAVolume", "0x00000000", cables, {"velocity": 127}),
            -60,
        )
        # near-silent (< -40 dB re peak) only at the soft end, as one run starting at velocity 1
        silent = dp.Patcher.silent_velocities("oscBVolume", "0x00000000", cables)
        self.assertEqual(silent, list(range(1, silent[-1] + 1)))
        self.assertLess(silent[-1], 20)

    def test_full_scale_cable_notches_mid_sweep(self):
        cables = [dp.PatchCable("velocity", "oscAVolume", 0x7FFFFFFF)]
        silent = dp.Patcher.silent_velocities("oscAVolume", "0x00000000", cables)
        self.assertTrue(silent)
        self.assertTrue(1 < min(silent) < 127)  # a notch in the middle, not at an end
        # ... and the sign flips past it: still audible at velocity 1
        self.assertGreater(
            dp.Patcher.evaluate("oscAVolume", "0x00000000", cables, {"velocity": 1}), 0
        )

    def test_parked_osc_cannot_be_patched_up(self):
        cables = [dp.PatchCable("velocity", "oscAVolume", 0x40000000)]
        self.assertEqual(
            len(dp.Patcher.silent_velocities("oscAVolume", "0x80000000", cables)), 127
        )

    def test_pitch_cable_squared(self):
        # memory calibration: 0x035E8000 random->pitch = +/- 9.4 cents
        c = [dp.PatchCable("random", "pitch", 0x035E8000)]
        cents = (
            dp.Patcher.pitch_semitones("pitch", "0x00000000", c, {"random": 1.0}) * 100
        )
        self.assertAlmostEqual(cents, 9.4, places=1)
        # and the inverse lands within a few LSB of the hand-solved amount
        self.assertAlmostEqual(
            dp.s32(dp.pitch_cable_amount(0.094)) / 0x035E8000, 1.0, places=2
        )
        # preset space: one octave = 2^29
        self.assertAlmostEqual(
            dp.Patcher.pitch_semitones("pitch", dp.semitones(12), [], {}),
            12.0,
            places=3,
        )

    def test_velocity_to_volume_stock_cable(self):
        # LOCAL_VOLUME preset is 0; the stock 0x3FFFFFE8 cable gives ~-12 dB at 64 and ~0 at 127
        c = [dp.PatchCable("velocity", "volume", "0x3FFFFFE8")]
        self.assertAlmostEqual(
            dp.Patcher.gain_db("volume", 0, c, {"velocity": 64}), -12.04, places=2
        )
        self.assertGreater(dp.Patcher.gain_db("volume", 0, c, {"velocity": 127}), -0.2)


class SampleRanges(unittest.TestCase):
    def test_last_range_must_be_open(self):
        with self.assertRaisesRegex(dp.PresetError, "omit"):
            dp.sample_ranges([("a.wav", 60), ("b.wav", 72)])
        with self.assertRaisesRegex(dp.PresetError, "omit"):
            dp.sample_ranges([("a.wav", None), ("b.wav", None)])

    def test_duplicate_tops_rejected(self):
        with self.assertRaisesRegex(dp.PresetError, "FILE_CORRUPTED"):
            dp.sample_ranges([("a.wav", 60), ("b.wav", 60), ("c.wav", None)])

    def test_sorted_and_open_last(self):
        rs = dp.sample_ranges([("c.wav", None), ("b.wav", 72), ("a.wav", 60)])
        self.assertEqual([r.file_name for r in rs], ["a.wav", "b.wav", "c.wav"])
        self.assertIsNone(rs[-1].top)

    def test_bounds_range(self):
        with self.assertRaises(dp.PresetError):
            dp.sample_ranges([("a.wav", 200), ("b.wav", None)])


class XmlShape(unittest.TestCase):
    def test_sound_root_and_order(self):
        s = dp.Sound(name="T")
        xml = s.to_xml()
        self.assertTrue(
            xml.startswith(
                f'<sound\n\tfirmwareVersion="{dp.FIRMWARE_VERSION}"\n\tearliestCompatibleFirmware="{dp.EARLIEST_COMPATIBLE_FIRMWARE}"'
            )
        )
        order = [
            t
            for t in (
                "<osc1",
                "<osc2",
                "<lfo1",
                "<lfo4",
                "<unison",
                "<defaultParams",
                "<envelope1",
                "<envelope4",
                "<equalizer",
                "</defaultParams>",
                "<modKnobs>",
                "<delay",
                "<sidechain",
            )
        ]
        pos = [xml.index(t) for t in order]
        self.assertEqual(pos, sorted(pos))
        # defaultParams attribute order is the serializer's
        dp_start = xml.index("<defaultParams")
        attrs = [
            ln.strip().split("=")[0]
            for ln in xml[dp_start:].split("\n")[1:]
            if "=" in ln and "<" not in ln
        ]
        self.assertEqual(
            tuple(attrs[: len(dp.SOUND_PARAM_NAMES)]), dp.SOUND_PARAM_NAMES
        )

    def test_drum_row_has_name_first_and_no_version(self):
        s = dp.Sound(name="KICK", polyphonic="auto")
        xml = s.to_xml(as_drum=True, indent=2)
        self.assertTrue(
            xml.startswith('\t\t<sound\n\t\t\tname="KICK"\n\t\t\tpolyphonic="auto"')
        )
        self.assertNotIn("firmwareVersion", xml)

    def test_sample_osc_single_vs_ranges(self):
        o = dp.Osc().set_sample("SAMPLES/x.wav", loop_mode=dp.LOOP_MODE_ONCE)
        xml = o.to_xml("osc1", 1)
        self.assertNotIn("sampleRanges", xml)
        self.assertIn('fileName="SAMPLES/x.wav"', xml)
        self.assertIn('loopMode="1"', xml)
        self.assertIn('<zone startSamplePos="0" endSamplePos="0" />', xml)
        o = dp.Osc().set_ranges(
            [("SAMPLES/a.wav", 60, 60 - 48), ("SAMPLES/b.wav", None, 60 - 72)]
        )
        xml = o.to_xml("osc1", 1)
        self.assertIn("<sampleRanges>", xml)
        self.assertEqual(xml.count("<sampleRange"), 2 + 1)  # 2 ranges + the wrapper
        self.assertEqual(
            xml.count("rangeTopNote"), 1
        )  # only the first range is bounded
        self.assertIn('transpose="12"', xml)
        self.assertIn('transpose="-12"', xml)
        o = dp.Osc().set_ranges(
            [("SAMPLES/a.wav", 63), ("SAMPLES/b.wav", None)], keyed="velocity"
        )
        self.assertIn('rangeTopVelocity="63"', o.to_xml("osc1", 1))

    def test_drum_model_osc(self):
        o = dp.Osc("drum", transpose=-3, drum_model="808kick")
        self.assertIn('drumModel="808kick"', o.to_xml("osc1", 1))
        with self.assertRaises(dp.PresetError):
            dp.Osc("drum", drum_model="bongo")

    def test_kit_shape(self):
        k = dp.Kit()
        k.drum("KICK").osc1.set_sample("SAMPLES/k.wav")
        k.drum("SNARE").osc1.set_sample("SAMPLES/s.wav")
        xml = k.to_xml()
        self.assertTrue(
            xml.startswith(f'<kit\n\tfirmwareVersion="{dp.FIRMWARE_VERSION}"')
        )
        self.assertIn("<soundSources>", xml)
        self.assertEqual(xml.count("<sound\n"), 2)
        self.assertTrue(xml.index('name="KICK"') < xml.index('name="SNARE"'))
        self.assertIn("<selectedDrumIndex>0</selectedDrumIndex>", xml)
        self.assertIn(
            '<patchCable source="velocity" destination="volume" polarity="bipolar" amount="0x3FFFFFE8" />',
            xml,
        )

    def test_round_trip_flatten(self):
        s = dp.Sound(name="RT")
        s.set(volume=dp.knob(30))
        flat = dp.flatten_preset_xml(s.xml_document())
        self.assertEqual(flat["sound/defaultParams/volume"], dp.knob(30))
        self.assertEqual(flat["sound/firmwareVersion"], dp.FIRMWARE_VERSION)

    def test_escaping(self):
        o = dp.Osc().set_sample('SAMPLES/A & B "x".wav')
        self.assertIn("A &amp; B &quot;x&quot;", o.to_xml("osc1", 1))


class Validation(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="dp_test_")

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def test_bad_hex_and_unipolar(self):
        s = dp.Sound()
        with self.assertRaises(dp.PresetError):
            s.set(volume="0x1234")
        with self.assertRaises(dp.PresetError):
            s.set(notAParam="0x00000000")
        s.params["oscAPulseWidth"] = "0x80000000"  # bypass set() to plant a bad value
        rep = s.validate()
        self.assertFalse(rep.ok)
        self.assertTrue(any("unipolar" in e for e in rep.errors))

    def test_missing_sample_file(self):
        s = dp.Sound()
        s.osc1.set_sample("SAMPLES/nope.wav")
        rep = s.validate(self.root)
        self.assertTrue(any("missing sample" in e for e in rep.errors))
        dp.write_silent_wav(os.path.join(self.root, "SAMPLES/nope.wav"))
        self.assertTrue(s.validate(self.root).ok)

    def test_flac_rejected(self):
        s = dp.Sound()
        s.osc1.set_sample("SAMPLES/x.flac")
        rep = s.validate()
        self.assertTrue(any("FLAC" in e for e in rep.errors))

    def test_velocity_ladder_must_ascend(self):
        files = ["SAMPLES/L/v1.wav", "SAMPLES/L/v2.wav", "SAMPLES/L/v3.wav"]
        for f, lvl in zip(files, (-20.0, -6.0, -12.0)):  # v3 quieter than v2
            dp.write_tone_wav(os.path.join(self.root, f), peak_dbfs=lvl)
        s = dp.Sound(name="SN")
        s.osc1.set_ranges(
            [(files[0], 40), (files[1], 90), (files[2], None)], keyed="velocity"
        )
        rep = s.validate(self.root)
        self.assertTrue(any("ladder does not ascend" in e for e in rep.errors))
        # explicit level_db overrides measuring
        s.osc1.ranges[2].level_db = -1.0
        s.osc1.ranges[1].level_db = -6.0
        s.osc1.ranges[0].level_db = -20.0
        self.assertTrue(s.validate(self.root).ok)

    def test_parked_osc_with_cable_is_an_error(self):
        s = dp.Sound()
        s.set(oscBVolume="0x80000000")
        s.cable("velocity", "oscBVolume", 0x40000000)
        rep = s.validate()
        self.assertTrue(any("parked at 0x80000000" in e for e in rep.errors))

    def test_full_scale_cable_warns_about_silence(self):
        s = dp.Sound()
        s.set(oscAVolume="0x00000000")
        s.cable("velocity", "oscAVolume", 0x7FFFFFFF)
        rep = s.validate()
        self.assertTrue(rep.ok)
        self.assertTrue(
            any("driven through zero at velocities" in w for w in rep.warnings)
        )
        self.assertTrue(any("exceeds" in w for w in rep.warnings))

    def test_safe_crossfade_only_notes_the_extremes(self):
        s = dp.Sound()
        s.set(oscAVolume="0x00000000", oscBVolume="0x00000000")
        s.cable("velocity", "oscAVolume", (-0x40000000) & 0xFFFFFFFF)
        s.cable("velocity", "oscBVolume", 0x40000000)
        rep = s.validate()
        self.assertTrue(rep.ok)
        # oscB's soft end is the ordinary fade (a note); oscA being silenced at the LOUD end
        # is what the sweep is there to point out (a warning)
        self.assertTrue(any("oscBVolume fades" in n for n in rep.notes))
        self.assertTrue(
            any("oscAVolume is silenced at velocity >=" in w for w in rep.warnings)
        )
        self.assertFalse(any("through zero" in w for w in rep.warnings))

    def test_kit_validate_merges_rows(self):
        k = dp.Kit()
        k.drum("A").osc1.set_sample("SAMPLES/a.wav")
        k.drum("B").osc1.set_ranges(
            [("SAMPLES/b1.wav", 60), ("SAMPLES/b1.wav", None)], keyed="velocity"
        )
        rep = k.validate(self.root, check_ladders=False)
        self.assertFalse(rep.ok)
        self.assertTrue(any("missing sample" in e for e in rep.errors))
        self.assertTrue(any("FORK ONLY" in n for n in rep.notes))
        with self.assertRaises(dp.PresetError):
            rep.raise_on_error()


class WavHelpers(unittest.TestCase):
    def test_peak_and_unity_note(self):
        d = tempfile.mkdtemp(prefix="dp_wav_")
        try:
            p = dp.write_tone_wav(
                os.path.join(d, "t.wav"), peak_dbfs=-6.0, unity_note=57
            )
            self.assertAlmostEqual(dp.wav_peak_dbfs(p), -6.0, places=1)
            self.assertEqual(dp.wav_unity_note(p), 57)
            self.assertEqual(dp.stamp_root_note(p, 60), "patched")
            self.assertEqual(dp.wav_unity_note(p), 60)
            info = dp.wav_info(p)
            self.assertEqual((info["rate"], info["bits"]), (44100, 16))
        finally:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
