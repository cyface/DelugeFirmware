/*
 * Copyright © 2026 Tim White
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "definitions_cxx.hpp"
#include "dsp/drums/analog_bass_drum.h"
#include "dsp/drums/analog_snare_drum.h"
#include "dsp/drums/hi_hat.h"
#include "dsp/drums/overdrive.h"
#include "dsp/drums/synthetic_bass_drum.h"
#include "dsp/drums/synthetic_snare_drum.h"
#include <cstdint>

namespace deluge::dsp::drums {

/// Per-voice (per unison part) state for the OscType::DRUM oscillator: one of the Mutable Instruments Plaits
/// drum models, plus the trigger / accent bookkeeping that Plaits' voice wrapper would otherwise do.
///
/// The three macro parameters follow Plaits' naming: `tone` is TIMBRE, `decay` is MORPH and `harmonics` is
/// the model-specific third control (attack FM + overdrive on the kicks, snappy on the snares, noisiness on
/// the hi-hat). All are 0..1. `f0` is the fundamental in cycles per sample.
class DrumVoice {
public:
	/// Fully resets the model state and arms a trigger for the next render() call.
	void init(DrumModel model, uint8_t velocity);

	/// Re-arms the trigger without clearing the resonators, like hitting a real drum that is still ringing.
	void retrigger(uint8_t velocity);

	/// Renders `numSamples` mono samples (nominal range ±1) into `out`. Returns false once the drum has decayed
	/// to silence, at which point the caller should release the voice.
	bool render(float* out, int32_t numSamples, float f0, float tone, float decay, float harmonics);

	DrumModel model() const { return model_; }

private:
	DrumModel model_;
	bool triggerPending_;
	uint8_t silentBlocks_;
	float accent_;

	union {
		AnalogBassDrum analogKick;
		AnalogSnareDrum analogSnare;
		HiHat<SquareNoise, SwingVCA, true, false> hiHat;
		SyntheticBassDrum synthKick;
		SyntheticSnareDrum synthSnare;
		HiHat<RingModNoise, LinearVCA, false, true> hiHat2;
	} model_state_;
	Overdrive overdrive_;
};

/// Pitch offset applied to the note so a model played at the Deluge's default note (C3 in a kit row) lands in
/// its classic register: kicks two octaves down, snares one octave down, hi-hat as-is.
float drumModelPitchScale(DrumModel model);

DrumVoice* solicitDrumVoice();
void drumVoiceUnassigned(DrumVoice* voice);

} // namespace deluge::dsp::drums
