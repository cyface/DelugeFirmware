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
// Band-limited (polyBLEP) saw / square oscillator, reduced from
// plaits/dsp/oscillator/oscillator.h (+ stmlib/dsp/polyblep.h) to the two shapes
// the ring-modulated hi-hat noise source uses; no external FM.

#pragma once

#include "dsp/drums/stmlib.h"
#include <cmath>
#include <cstddef>

namespace deluge::dsp::drums {

enum OscillatorShape { OSCILLATOR_SHAPE_SAW, OSCILLATOR_SHAPE_SQUARE };

inline float ThisBlepSample(float t) {
	return 0.5f * t * t;
}

inline float NextBlepSample(float t) {
	t = 1.0f - t;
	return -0.5f * t * t;
}

class Oscillator {
public:
	static constexpr float kMaxFrequency = 0.25f;
	static constexpr float kMinFrequency = 0.000001f;

	void Init() {
		phase_ = 0.5f;
		next_sample_ = 0.0f;
		high_ = true;
		frequency_ = 0.001f;
		pw_ = 0.5f;
	}

	template <OscillatorShape shape>
	void Render(float frequency, float pw, float* out, size_t size) {
		DRUMS_CONSTRAIN(frequency, kMinFrequency, kMaxFrequency);
		DRUMS_CONSTRAIN(pw, frequency * 2.0f, 1.0f - 2.0f * frequency)

		ParameterInterpolator fm(&frequency_, frequency, size);
		ParameterInterpolator pwm(&pw_, pw, size);

		float next_sample = next_sample_;

		while (size--) {
			float this_sample = next_sample;
			next_sample = 0.0f;

			const float f = fm.Next();
			const float w = pwm.Next();
			phase_ += f;

			if constexpr (shape == OSCILLATOR_SHAPE_SAW) {
				if (phase_ >= 1.0f) {
					phase_ -= 1.0f;
					float t = phase_ / f;
					this_sample -= ThisBlepSample(t);
					next_sample -= NextBlepSample(t);
				}
				next_sample += phase_;
				*out++ = 2.0f * this_sample - 1.0f;
			}
			else {
				if (high_ ^ (phase_ >= w)) {
					float t = (phase_ - w) / f;
					this_sample += ThisBlepSample(t);
					next_sample += NextBlepSample(t);
					high_ = phase_ >= w;
				}
				if (phase_ >= 1.0f) {
					phase_ -= 1.0f;
					float t = phase_ / f;
					this_sample -= ThisBlepSample(t);
					next_sample -= NextBlepSample(t);
					high_ = false;
				}
				next_sample += phase_ < w ? 0.0f : 1.0f;
				*out++ = 2.0f * this_sample - 1.0f;
			}
		}
		next_sample_ = next_sample;
	}

private:
	float phase_;
	float next_sample_;
	bool high_;
	float frequency_;
	float pw_;
};

} // namespace deluge::dsp::drums
