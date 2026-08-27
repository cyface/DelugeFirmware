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
// 808 HH, with a few extra parameters to push things to the CY territory...
// The template parameter MetallicNoiseSource allows another kind of "metallic
// noise" to be used, for results which are more similar to KR-55 or FM hi-hats.
// (plaits/dsp/drums/hi_hat.h - the RingModNoise variant is not ported.)

#pragma once

#include "dsp/drums/dsp.h"
#include "dsp/drums/stmlib.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp::drums {

class SquareNoise {
public:
	void Init() { std::fill(&phase_[0], &phase_[6], 0); }

	void Render(float f0, float* out, size_t size) {
		const float ratios[6] = {// Nominal f0: 414 Hz
		                         1.0f, 1.304f, 1.466f, 1.787f, 1.932f, 2.536f};

		uint32_t increment[6];
		uint32_t phase[6];
		for (int i = 0; i < 6; ++i) {
			float f = f0 * ratios[i];
			if (f >= 0.499f) {
				f = 0.499f;
			}
			increment[i] = static_cast<uint32_t>(f * 4294967296.0f);
			phase[i] = phase_[i];
		}

		while (size--) {
			phase[0] += increment[0];
			phase[1] += increment[1];
			phase[2] += increment[2];
			phase[3] += increment[3];
			phase[4] += increment[4];
			phase[5] += increment[5];
			uint32_t noise = 0;
			noise += (phase[0] >> 31);
			noise += (phase[1] >> 31);
			noise += (phase[2] >> 31);
			noise += (phase[3] >> 31);
			noise += (phase[4] >> 31);
			noise += (phase[5] >> 31);
			*out++ = 0.33f * static_cast<float>(noise) - 1.0f;
		}

		for (int i = 0; i < 6; ++i) {
			phase_[i] = phase[i];
		}
	}

private:
	uint32_t phase_[6];
};

class SwingVCA {
public:
	float operator()(float s, float gain) {
		s *= s > 0.0f ? 4.0f : 0.1f;
		s = s / (1.0f + fabsf(s));
		return (s + 0.1f) * gain;
	}
};

class LinearVCA {
public:
	float operator()(float s, float gain) { return s * gain; }
};

template <typename MetallicNoiseSource, typename VCA, bool resonance, bool two_stage_envelope>
class HiHat {
public:
	void Init() {
		envelope_ = 0.0f;
		noise_clock_ = 0.0f;
		noise_sample_ = 0.0f;
		sustain_gain_ = 0.0f;

		metallic_noise_.Init();
		noise_coloration_svf_.Init();
		hpf_.Init();
	}

	void Render(bool sustain, bool trigger, float accent, float f0, float tone, float decay, float noisiness,
	            float* out, size_t size) {
		const float envelope_decay = 1.0f - 0.003f * SemitonesToRatio(-decay * 84.0f);
		const float cut_decay = 1.0f - 0.0025f * SemitonesToRatio(-decay * 36.0f);

		if (trigger) {
			envelope_ = (1.5f + 0.5f * (1.0f - decay)) * (0.3f + 0.7f * accent);
		}

		// Render the metallic noise.
		metallic_noise_.Render(2.0f * f0, out, size);

		// Apply BPF on the metallic noise.
		float cutoff = 150.0f / kSampleRate * SemitonesToRatio(tone * 72.0f);
		DRUMS_CONSTRAIN(cutoff, 0.0f, 16000.0f / kSampleRate);
		noise_coloration_svf_.set_f_q<FREQUENCY_ACCURATE>(cutoff, resonance ? 3.0f + 3.0f * tone : 1.0f);
		noise_coloration_svf_.Process<FILTER_MODE_BAND_PASS>(out, out, size);

		// This is not at all part of the 808 circuit! But to add more variety, we
		// add a variable amount of clocked noise to the output of the 6 schmitt
		// trigger oscillators.
		noisiness *= noisiness;
		float noise_f = f0 * (16.0f + 16.0f * (1.0f - noisiness));
		DRUMS_CONSTRAIN(noise_f, 0.0f, 0.5f);

		for (size_t i = 0; i < size; ++i) {
			noise_clock_ += noise_f;
			if (noise_clock_ >= 1.0f) {
				noise_clock_ -= 1.0f;
				noise_sample_ = Random::GetFloat() - 0.5f;
			}
			out[i] += noisiness * (noise_sample_ - out[i]);
		}

		// Apply VCA.
		ParameterInterpolator sustain_gain(&sustain_gain_, accent * decay, size);
		for (size_t i = 0; i < size; ++i) {
			VCA vca;
			envelope_ *= envelope_ > 0.5f || !two_stage_envelope ? envelope_decay : cut_decay;
			out[i] = vca(out[i], sustain ? sustain_gain.Next() : envelope_);
		}

		hpf_.set_f_q<FREQUENCY_ACCURATE>(cutoff, 0.5f);
		hpf_.Process<FILTER_MODE_HIGH_PASS>(out, out, size);
	}

private:
	float envelope_;
	float noise_clock_;
	float noise_sample_;
	float sustain_gain_;

	MetallicNoiseSource metallic_noise_;
	Svf noise_coloration_svf_;
	Svf hpf_;
};

} // namespace deluge::dsp::drums
