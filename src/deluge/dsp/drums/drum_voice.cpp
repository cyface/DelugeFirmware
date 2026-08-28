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

#include "dsp/drums/drum_voice.h"
#include "memory/memory_allocator_interface.h"
#include <algorithm>
#include <cmath>
#include <new>

namespace deluge::dsp::drums {

float lut_sine[kSineLUTEntries];

void initSineTable() {
	static bool done = false;
	if (done) {
		return;
	}
	for (size_t i = 0; i < kSineLUTEntries; i++) {
		lut_sine[i] = sinf(2.0f * kPi * static_cast<float>(i) / kSineLUTSize);
	}
	done = true;
}

namespace {
// Below this peak level (about -86 dBFS) for kSilentBlocksToRelease consecutive blocks the drum is considered
// finished and the voice is released, the way a one-shot sample ends when it runs out.
constexpr float kSilenceThreshold = 0.00005f;
constexpr uint8_t kSilentBlocksToRelease = 4;
} // namespace

float drumModelPitchScale(DrumModel model) {
	switch (model) {
	case DrumModel::ANALOG_KICK:
	case DrumModel::SYNTH_KICK:
		return 0.25f;
	case DrumModel::ANALOG_SNARE:
	case DrumModel::SYNTH_SNARE:
		return 0.5f;
	default:
		return 1.0f;
	}
}

void DrumVoice::init(DrumModel model, uint8_t velocity) {
	model_ = model;
	switch (model_) {
	case DrumModel::ANALOG_KICK:
		model_state_.analogKick.Init();
		overdrive_.Init();
		break;
	case DrumModel::ANALOG_SNARE:
		model_state_.analogSnare.Init();
		break;
	case DrumModel::HI_HAT:
		model_state_.hiHat.Init();
		break;
	case DrumModel::SYNTH_KICK:
		model_state_.synthKick.Init();
		break;
	case DrumModel::SYNTH_SNARE:
		model_state_.synthSnare.Init();
		break;
	case DrumModel::HI_HAT_2:
		model_state_.hiHat2.Init();
		break;
	}
	retrigger(velocity);
}

void DrumVoice::retrigger(uint8_t velocity) {
	accent_ = static_cast<float>(std::min<uint8_t>(velocity, 127)) * (1.0f / 127.0f);
	triggerPending_ = true;
	silentBlocks_ = 0;
}

bool DrumVoice::render(float* out, int32_t numSamples, float f0, float tone, float decay, float harmonics) {
	const bool trigger = triggerPending_;
	triggerPending_ = false;
	const size_t size = static_cast<size_t>(numSamples);

	// Parameter mapping follows plaits/dsp/engine/{bass_drum,snare_drum,hi_hat}_engine.cc
	switch (model_) {
	case DrumModel::ANALOG_KICK: {
		const float attack_fm_amount = std::min(harmonics * 4.0f, 1.0f);
		const float self_fm_amount = std::clamp(harmonics * 4.0f - 1.0f, 0.0f, 1.0f);
		const float drive = std::max(harmonics * 2.0f - 1.0f, 0.0f) * std::max(1.0f - 16.0f * f0, 0.0f);
		model_state_.analogKick.Render(false, trigger, accent_, f0, tone, decay, attack_fm_amount, self_fm_amount, out,
		                               size);
		overdrive_.Process(0.5f + 0.5f * drive, out, size);
		break;
	}
	case DrumModel::ANALOG_SNARE:
		model_state_.analogSnare.Render(false, trigger, accent_, f0, tone, decay, harmonics, out, size);
		break;
	case DrumModel::HI_HAT:
		model_state_.hiHat.Render(false, trigger, accent_, f0, tone, decay, harmonics, out, size);
		break;
	case DrumModel::SYNTH_KICK:
		model_state_.synthKick.Render(false, trigger, accent_, f0, tone, decay, 0.4f - 0.25f * decay * decay,
		                              std::min(harmonics * 2.0f, 1.0f), std::max(harmonics * 2.0f - 1.0f, 0.0f), out,
		                              size);
		break;
	case DrumModel::SYNTH_SNARE:
		model_state_.synthSnare.Render(false, trigger, accent_, f0, tone, decay, harmonics, out, size);
		break;
	case DrumModel::HI_HAT_2:
		model_state_.hiHat2.Render(false, trigger, accent_, f0, tone, decay, harmonics, out, size);
		break;
	}

	if (trigger) {
		return true;
	}

	float peak = 0.0f;
	for (size_t i = 0; i < size; i++) {
		peak = std::max(peak, fabsf(out[i]));
	}
	if (peak >= kSilenceThreshold) {
		silentBlocks_ = 0;
		return true;
	}
	if (silentBlocks_ < kSilentBlocksToRelease) {
		silentBlocks_++;
		return true;
	}
	return false;
}

DrumVoice* solicitDrumVoice() {
	initSineTable();
	void* memory = allocMaxSpeed(sizeof(DrumVoice));
	if (memory == nullptr) {
		return nullptr;
	}
	return new (memory) DrumVoice();
}

void drumVoiceUnassigned(DrumVoice* voice) {
	delugeDealloc(voice);
}

} // namespace deluge::dsp::drums
