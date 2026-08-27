// Copyright 2014 Emilie Gillet.
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
// The subset of stmlib (https://github.com/pichenettes/stmlib) that the Plaits
// drum models depend on: one-pole / state-variable filters, block parameter
// interpolation, soft clipping, pitch-ratio conversion and the LCG noise source.
// Adapted for the Deluge: LUT-based SemitonesToRatio replaced by exp2f (it is only
// evaluated once per render block), inline-asm sqrt replaced by sqrtf, and the
// classes made trivially constructible so they can live inside a union.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace deluge::dsp::drums {

#define DRUMS_MAKE_INTEGRAL_FRACTIONAL(x)                                                                              \
	int32_t x##_integral = static_cast<int32_t>(x);                                                                    \
	float x##_fractional = x - static_cast<float>(x##_integral);

#define DRUMS_ONE_POLE(out, in, coefficient) out += (coefficient) * ((in) - out);

#define DRUMS_SLOPE(out, in, positive, negative)                                                                       \
	{                                                                                                                  \
		float error = (in) - out;                                                                                      \
		out += (error > 0 ? positive : negative) * error;                                                              \
	}

#define DRUMS_CONSTRAIN(var, min, max)                                                                                 \
	if (var < (min)) {                                                                                                 \
		var = (min);                                                                                                   \
	}                                                                                                                  \
	else if (var > (max)) {                                                                                            \
		var = (max);                                                                                                   \
	}

inline float Interpolate(const float* table, float index, float size) {
	index *= size;
	DRUMS_MAKE_INTEGRAL_FRACTIONAL(index)
	float a = table[index_integral];
	float b = table[index_integral + 1];
	return a + (b - a) * index_fractional;
}

inline float InterpolateWrap(const float* table, float index, float size) {
	index -= static_cast<float>(static_cast<int32_t>(index));
	index *= size;
	DRUMS_MAKE_INTEGRAL_FRACTIONAL(index)
	float a = table[index_integral];
	float b = table[index_integral + 1];
	return a + (b - a) * index_fractional;
}

inline float SoftLimit(float x) {
	return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

inline float SoftClip(float x) {
	if (x < -3.0f) {
		return -1.0f;
	}
	if (x > 3.0f) {
		return 1.0f;
	}
	return SoftLimit(x);
}

inline float Sqrt(float x) {
	return sqrtf(x);
}

inline float SemitonesToRatio(float semitones) {
	return exp2f(semitones * (1.0f / 12.0f));
}

/// Linear-congruential noise source (same constants as stmlib::Random).
class Random {
public:
	static inline uint32_t GetWord() {
		rng_state_ = rng_state_ * 1664525L + 1013904223L;
		return rng_state_;
	}
	static inline float GetFloat() { return static_cast<float>(GetWord()) / 4294967296.0f; }

private:
	static inline uint32_t rng_state_ = 0x21;
};

class ParameterInterpolator {
public:
	ParameterInterpolator(float* state, float new_value, size_t size) { Init(state, new_value, size); }
	~ParameterInterpolator() { *state_ = value_; }

	inline void Init(float* state, float new_value, size_t size) {
		state_ = state;
		value_ = *state;
		increment_ = (new_value - *state) / static_cast<float>(size);
	}
	inline float Next() {
		value_ += increment_;
		return value_;
	}

private:
	float* state_;
	float value_;
	float increment_;
};

enum FilterMode {
	FILTER_MODE_LOW_PASS,
	FILTER_MODE_BAND_PASS,
	FILTER_MODE_BAND_PASS_NORMALIZED,
	FILTER_MODE_HIGH_PASS
};

enum FrequencyApproximation { FREQUENCY_EXACT, FREQUENCY_ACCURATE, FREQUENCY_FAST, FREQUENCY_DIRTY };

constexpr float kPi = 3.14159265358979323846f;
constexpr float kPiPow2 = kPi * kPi;
constexpr float kPiPow3 = kPiPow2 * kPi;
constexpr float kPiPow5 = kPiPow3 * kPiPow2;
constexpr float kPiPow7 = kPiPow5 * kPiPow2;
constexpr float kPiPow9 = kPiPow7 * kPiPow2;
constexpr float kPiPow11 = kPiPow9 * kPiPow2;

class OnePole {
public:
	void Init() {
		set_f<FREQUENCY_DIRTY>(0.01f);
		Reset();
	}

	void Reset() { state_ = 0.0f; }

	template <FrequencyApproximation approximation>
	static inline float tan(float f) {
		if constexpr (approximation == FREQUENCY_EXACT) {
			// Clip coefficient to about 100.
			f = f < 0.497f ? f : 0.497f;
			return tanf(kPi * f);
		}
		else if constexpr (approximation == FREQUENCY_DIRTY) {
			// Optimized for frequencies below 8kHz.
			const float a = 3.736e-01f * kPiPow3;
			return f * (kPi + a * f * f);
		}
		else if constexpr (approximation == FREQUENCY_FAST) {
			// The usual tangent approximation uses 3.1755e-01 and 2.033e-01, but
			// the coefficients used here are optimized to minimize error for the
			// 16Hz to 16kHz range, with a sample rate of 48kHz.
			const float a = 3.260e-01f * kPiPow3;
			const float b = 1.823e-01f * kPiPow5;
			float f2 = f * f;
			return f * (kPi + f2 * (a + b * f2));
		}
		else {
			// These coefficients don't need to be tweaked for the audio range.
			const float a = 3.333314036e-01f * kPiPow3;
			const float b = 1.333923995e-01f * kPiPow5;
			const float c = 5.33740603e-02f * kPiPow7;
			const float d = 2.900525e-03f * kPiPow9;
			const float e = 9.5168091e-03f * kPiPow11;
			float f2 = f * f;
			return f * (kPi + f2 * (a + f2 * (b + f2 * (c + f2 * (d + f2 * e)))));
		}
	}

	template <FrequencyApproximation approximation>
	inline void set_f(float f) {
		g_ = tan<approximation>(f);
		gi_ = 1.0f / (1.0f + g_);
	}

	template <FilterMode mode>
	inline float Process(float in) {
		float lp = (g_ * in + state_) * gi_;
		state_ = g_ * (in - lp) + lp;
		if constexpr (mode == FILTER_MODE_LOW_PASS) {
			return lp;
		}
		else if constexpr (mode == FILTER_MODE_HIGH_PASS) {
			return in - lp;
		}
		else {
			return 0.0f;
		}
	}

private:
	float g_;
	float gi_;
	float state_;
};

class Svf {
public:
	void Init() {
		set_f_q<FREQUENCY_DIRTY>(0.01f, 100.0f);
		Reset();
	}

	void Reset() { state_1_ = state_2_ = 0.0f; }

	// Set frequency and resonance from true units. Various approximations
	// are available to avoid the cost of tanf.
	template <FrequencyApproximation approximation>
	inline void set_f_q(float f, float resonance) {
		g_ = OnePole::tan<approximation>(f);
		r_ = 1.0f / resonance;
		h_ = 1.0f / (1.0f + r_ * g_ + g_ * g_);
	}

	template <FilterMode mode>
	inline float Process(float in) {
		float hp, bp, lp;
		hp = (in - r_ * state_1_ - g_ * state_1_ - state_2_) * h_;
		bp = g_ * hp + state_1_;
		state_1_ = g_ * hp + bp;
		lp = g_ * bp + state_2_;
		state_2_ = g_ * bp + lp;
		return select<mode>(lp, bp, hp);
	}

	template <FilterMode mode_1, FilterMode mode_2>
	inline void Process(float in, float* out_1, float* out_2) {
		float hp, bp, lp;
		hp = (in - r_ * state_1_ - g_ * state_1_ - state_2_) * h_;
		bp = g_ * hp + state_1_;
		state_1_ = g_ * hp + bp;
		lp = g_ * bp + state_2_;
		state_2_ = g_ * bp + lp;
		*out_1 = select<mode_1>(lp, bp, hp);
		*out_2 = select<mode_2>(lp, bp, hp);
	}

	template <FilterMode mode>
	inline void Process(const float* in, float* out, size_t size) {
		float hp, bp, lp;
		float state_1 = state_1_;
		float state_2 = state_2_;
		while (size--) {
			hp = (*in - r_ * state_1 - g_ * state_1 - state_2) * h_;
			bp = g_ * hp + state_1;
			state_1 = g_ * hp + bp;
			lp = g_ * bp + state_2;
			state_2 = g_ * bp + lp;
			*out++ = select<mode>(lp, bp, hp);
			++in;
		}
		state_1_ = state_1;
		state_2_ = state_2;
	}

private:
	template <FilterMode mode>
	inline float select(float lp, float bp, float hp) const {
		if constexpr (mode == FILTER_MODE_LOW_PASS) {
			return lp;
		}
		else if constexpr (mode == FILTER_MODE_BAND_PASS) {
			return bp;
		}
		else if constexpr (mode == FILTER_MODE_BAND_PASS_NORMALIZED) {
			return bp * r_;
		}
		else {
			return hp;
		}
	}

	float g_;
	float r_;
	float h_;
	float state_1_;
	float state_2_;
};

} // namespace deluge::dsp::drums
