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
// Plaits' dsp.h + sine_oscillator.h, reduced to what the drum models use.
// Adapted for the Deluge: the sample rate is 44.1 kHz (every time constant in the
// models is expressed in seconds × kSampleRate, so they re-derive automatically),
// and the sine LUT is filled at runtime by initSineTable() instead of shipping
// Plaits' resources.cc.

#pragma once

#include "dsp/drums/stmlib.h"
#include <cstddef>
#include <cstdint>

namespace deluge::dsp::drums {

constexpr float kSampleRate = 44100.0f;

constexpr float kSineLUTSize = 512.0f;
constexpr size_t kSineLUTEntries = 512 + 1;

extern float lut_sine[kSineLUTEntries];
void initSineTable();

inline float Sine(float phase) {
	return InterpolateWrap(lut_sine, phase, kSineLUTSize);
}

inline float SineNoWrap(float phase) {
	return Interpolate(lut_sine, phase, kSineLUTSize);
}

class SineOscillator {
public:
	void Init() {
		phase_ = 0.0f;
		frequency_ = 0.0f;
		amplitude_ = 0.0f;
	}

	inline float Next(float frequency) {
		if (frequency >= 0.5f) {
			frequency = 0.5f;
		}
		phase_ += frequency;
		if (phase_ >= 1.0f) {
			phase_ -= 1.0f;
		}
		return SineNoWrap(phase_);
	}

	inline void Next(float frequency, float amplitude, float* sin, float* cos) {
		if (frequency >= 0.5f) {
			frequency = 0.5f;
		}
		phase_ += frequency;
		if (phase_ >= 1.0f) {
			phase_ -= 1.0f;
		}
		*sin = amplitude * SineNoWrap(phase_);
		// The LUT only spans one cycle, so wrap the quadrature lookup explicitly.
		*cos = amplitude * Sine(phase_ + 0.25f);
	}

private:
	float phase_;
	float frequency_;
	float amplitude_;
};

} // namespace deluge::dsp::drums
