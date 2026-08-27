// Copyright 2016 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// 808 bass drum model, revisited. (plaits/dsp/drums/analog_bass_drum.h)

#pragma once

#include "dsp/drums/dsp.h"
#include "dsp/drums/stmlib.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp::drums {

class AnalogBassDrum {
public:
	void Init() {
		pulse_remaining_samples_ = 0;
		fm_pulse_remaining_samples_ = 0;
		pulse_ = 0.0f;
		pulse_height_ = 0.0f;
		pulse_lp_ = 0.0f;
		fm_pulse_lp_ = 0.0f;
		retrig_pulse_ = 0.0f;
		lp_out_ = 0.0f;
		tone_lp_ = 0.0f;
		sustain_gain_ = 0.0f;

		resonator_.Init();
		oscillator_.Init();
	}

	inline float Diode(float x) {
		if (x >= 0.0f) {
			return x;
		}
		x *= 2.0f;
		return 0.7f * x / (1.0f + fabsf(x));
	}

	void Render(bool sustain, bool trigger, float accent, float f0, float tone, float decay, float attack_fm_amount,
	            float self_fm_amount, float* out, size_t size) {
		const int kTriggerPulseDuration = 1.0e-3f * kSampleRate;
		const int kFMPulseDuration = 6.0e-3f * kSampleRate;
		const float kPulseDecayTime = 0.2e-3f * kSampleRate;
		const float kPulseFilterTime = 0.1e-3f * kSampleRate;
		const float kRetrigPulseDuration = 0.05f * kSampleRate;

		const float scale = 0.001f / f0;
		const float q = 1500.0f * SemitonesToRatio(decay * 80.0f);
		const float tone_f = std::min(4.0f * f0 * SemitonesToRatio(tone * 108.0f), 1.0f);
		const float exciter_leak = 0.08f * (tone + 0.25f);

		if (trigger) {
			pulse_remaining_samples_ = kTriggerPulseDuration;
			fm_pulse_remaining_samples_ = kFMPulseDuration;
			pulse_height_ = 3.0f + 7.0f * accent;
			lp_out_ = 0.0f;
		}

		ParameterInterpolator sustain_gain(&sustain_gain_, accent * decay, size);

		while (size--) {
			// Q39 / Q40
			float pulse = 0.0f;
			if (pulse_remaining_samples_) {
				--pulse_remaining_samples_;
				pulse = pulse_remaining_samples_ ? pulse_height_ : pulse_height_ - 1.0f;
				pulse_ = pulse;
			}
			else {
				pulse_ *= 1.0f - 1.0f / kPulseDecayTime;
				pulse = pulse_;
			}
			if (sustain) {
				pulse = 0.0f;
			}

			// C40 / R163 / R162 / D83
			DRUMS_ONE_POLE(pulse_lp_, pulse, 1.0f / kPulseFilterTime);
			pulse = Diode((pulse - pulse_lp_) + pulse * 0.044f);

			// Q41 / Q42
			float fm_pulse = 0.0f;
			if (fm_pulse_remaining_samples_) {
				--fm_pulse_remaining_samples_;
				fm_pulse = 1.0f;
				// C39 / C52
				retrig_pulse_ = fm_pulse_remaining_samples_ ? 0.0f : -0.8f;
			}
			else {
				// C39 / R161
				retrig_pulse_ *= 1.0f - 1.0f / kRetrigPulseDuration;
			}
			if (sustain) {
				fm_pulse = 0.0f;
			}
			DRUMS_ONE_POLE(fm_pulse_lp_, fm_pulse, 1.0f / kPulseFilterTime);

			// Q43 and R170 leakage
			float punch = 0.7f + Diode(10.0f * lp_out_ - 1.0f);

			// Q43 / R165
			float attack_fm = fm_pulse_lp_ * 1.7f * attack_fm_amount;
			float self_fm = punch * 0.08f * self_fm_amount;
			float f = f0 * (1.0f + attack_fm + self_fm);
			DRUMS_CONSTRAIN(f, 0.0f, 0.4f);

			float resonator_out;
			if (sustain) {
				oscillator_.Next(f, sustain_gain.Next(), &resonator_out, &lp_out_);
			}
			else {
				resonator_.set_f_q<FREQUENCY_DIRTY>(f, 1.0f + q * f);
				resonator_.Process<FILTER_MODE_BAND_PASS, FILTER_MODE_LOW_PASS>((pulse - retrig_pulse_ * 0.2f) * scale,
				                                                                &resonator_out, &lp_out_);
			}

			DRUMS_ONE_POLE(tone_lp_, pulse * exciter_leak + resonator_out, tone_f);

			*out++ = tone_lp_;
		}
	}

private:
	int pulse_remaining_samples_;
	int fm_pulse_remaining_samples_;
	float pulse_;
	float pulse_height_;
	float pulse_lp_;
	float fm_pulse_lp_;
	float retrig_pulse_;
	float lp_out_;
	float tone_lp_;
	float sustain_gain_;

	Svf resonator_;

	// Replace the resonator in "free running" (sustain) mode.
	SineOscillator oscillator_;
};

} // namespace deluge::dsp::drums
