/*
 * Copyright © 2026 Synthstrom Audible Ltd
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
 *
 * The drum models and their building blocks are ports of Mutable Instruments Plaits / stmlib:
 *
 * Copyright 2014-2016 Emilie Gillet.
 *
 * Author: Emilie Gillet (emilie.o.gillet@gmail.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * See http://creativecommons.org/licenses/MIT/ for more information.
 */

/*
 * Kernel rules kept here (see plugin/README.md): no globals or statics, so the sine table and exp2 come from the
 * host API and the noise generator lives in the voice; no rodata tables (a pointer to one is a relocation in a
 * blob), so the few small constant tables are written out as expressions; no integer division; float is fine
 * (VFP, hard-float ABI) but sqrt goes through the builtin so nothing is imported from libm.
 *
 * Every expression keeps the operand order of the C++ original so the port is bit-exact against it
 * (verified with a native harness: all six models, random macros / pitches / velocities, 0 mismatches).
 */

#include "plugin/source/plaits_drums.h"

#define SR 44100.0f
#define PD_PI 3.14159265358979323846f
#define PD_PI_POW2 (PD_PI * PD_PI)
#define PD_PI_POW3 (PD_PI_POW2 * PD_PI)
#define PD_PI_POW5 (PD_PI_POW3 * PD_PI_POW2)
#define PD_PI_POW7 (PD_PI_POW5 * PD_PI_POW2)
#define PD_PI_POW9 (PD_PI_POW7 * PD_PI_POW2)
#define PD_PI_POW11 (PD_PI_POW9 * PD_PI_POW2)

/* Below this peak level (about -86 dBFS) for kSilentBlocksToRelease consecutive blocks the drum is finished and
 * the host may release the voice, the way a one-shot sample ends when it runs out. */
#define PD_SILENCE_THRESHOLD 0.00005f
#define PD_SILENT_BLOCKS_TO_RELEASE 4u

/* --- small helpers ------------------------------------------------------------------------------------------ */

static inline float pd_min(float a, float b) {
	return a < b ? a : b;
}
static inline float pd_max(float a, float b) {
	return a > b ? a : b;
}
static inline float pd_clamp(float x, float lo, float hi) {
	return x < lo ? lo : (x > hi ? hi : x);
}
static inline float pd_fabs(float x) {
	return __builtin_fabsf(x);
}
static inline float pd_sqrt(float x) {
	return __builtin_sqrtf(x);
}

/* stmlib CONSTRAIN: clamp in place. */
#define PD_CONSTRAIN(var, lo, hi)                                                                                      \
	if ((var) < (lo)) {                                                                                                \
		(var) = (lo);                                                                                                  \
	}                                                                                                                  \
	else if ((var) > (hi)) {                                                                                           \
		(var) = (hi);                                                                                                  \
	}

#define PD_ONE_POLE(out, in, coefficient) (out) += (coefficient) * ((in) - (out));

#define PD_SLOPE(out, in, positive, negative)                                                                          \
	{                                                                                                                  \
		float error = (in) - (out);                                                                                    \
		(out) += (error > 0 ? (positive) : (negative)) * error;                                                        \
	}

static inline float pd_semitones_to_ratio(const DelugePluginHostApi* api, float semitones) {
	return api->exp2f(semitones * (1.0f / 12.0f));
}

static inline float pd_soft_limit(float x) {
	return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

static inline float pd_soft_clip(float x) {
	if (x < -3.0f) {
		return -1.0f;
	}
	if (x > 3.0f) {
		return 1.0f;
	}
	return pd_soft_limit(x);
}

/* Linear-interpolated sine from the host's table; `pd_sine` wraps the phase, `pd_sine_no_wrap` expects 0..1. */
static inline float pd_sine_no_wrap(const DelugePluginHostApi* api, float phase) {
	float index = phase * (float)DELUGE_PLUGIN_SINE_LUT_SIZE;
	int32_t index_integral = (int32_t)index;
	float index_fractional = index - (float)index_integral;
	float a = api->sineLut[index_integral];
	float b = api->sineLut[index_integral + 1];
	return a + (b - a) * index_fractional;
}

static inline float pd_sine(const DelugePluginHostApi* api, float phase) {
	phase -= (float)((int32_t)phase);
	return pd_sine_no_wrap(api, phase);
}

/* stmlib::Random, but per voice: the same LCG, seeded by the host. */
static inline uint32_t pd_random_word(PlaitsDrumsVoice* v) {
	v->rng = v->rng * 1664525u + 1013904223u;
	return v->rng;
}
static inline float pd_random_float(PlaitsDrumsVoice* v) {
	return (float)pd_random_word(v) / 4294967296.0f;
}

/* stmlib::ParameterInterpolator: linear ramp from the stored value to the new one over one block; `done` stores
 * where the ramp got to (the C++ destructor). */
typedef struct {
	float* state;
	float value;
	float increment;
} PdInterpolator;

static inline void pd_interp_init(PdInterpolator* p, float* state, float newValue, uint32_t size) {
	p->state = state;
	p->value = *state;
	p->increment = (newValue - *state) / (float)size;
}
static inline float pd_interp_next(PdInterpolator* p) {
	p->value += p->increment;
	return p->value;
}
static inline void pd_interp_done(PdInterpolator* p) {
	*p->state = p->value;
}

/* --- filters ------------------------------------------------------------------------------------------------- */

/* The tangent approximations of stmlib::OnePole::tan<>, one function per FrequencyApproximation used. */
static inline float pd_tan_dirty(float f) {
	/* Optimized for frequencies below 8kHz. */
	const float a = 3.736e-01f * PD_PI_POW3;
	return f * (PD_PI + a * f * f);
}

static inline float pd_tan_fast(float f) {
	/* Coefficients optimized to minimize error for the 16Hz to 16kHz range at 48kHz. */
	const float a = 3.260e-01f * PD_PI_POW3;
	const float b = 1.823e-01f * PD_PI_POW5;
	float f2 = f * f;
	return f * (PD_PI + f2 * (a + b * f2));
}

static inline float pd_tan_accurate(float f) {
	const float a = 3.333314036e-01f * PD_PI_POW3;
	const float b = 1.333923995e-01f * PD_PI_POW5;
	const float c = 5.33740603e-02f * PD_PI_POW7;
	const float d = 2.900525e-03f * PD_PI_POW9;
	const float e = 9.5168091e-03f * PD_PI_POW11;
	float f2 = f * f;
	return f * (PD_PI + f2 * (a + f2 * (b + f2 * (c + f2 * (d + f2 * e)))));
}

static inline void pd_onepole_set_g(PdOnePole* p, float g) {
	p->g = g;
	p->gi = 1.0f / (1.0f + g);
}
static inline void pd_onepole_init(PdOnePole* p) {
	pd_onepole_set_g(p, pd_tan_dirty(0.01f));
	p->state = 0.0f;
}
static inline float pd_onepole_lp(PdOnePole* p, float in) {
	float lp = (p->g * in + p->state) * p->gi;
	p->state = p->g * (in - lp) + lp;
	return lp;
}
static inline float pd_onepole_hp(PdOnePole* p, float in) {
	float lp = (p->g * in + p->state) * p->gi;
	p->state = p->g * (in - lp) + lp;
	return in - lp;
}

static inline void pd_svf_set_g_q(PdSvf* s, float g, float resonance) {
	s->g = g;
	s->r = 1.0f / resonance;
	s->h = 1.0f / (1.0f + s->r * s->g + s->g * s->g);
}
static inline void pd_svf_init(PdSvf* s) {
	pd_svf_set_g_q(s, pd_tan_dirty(0.01f), 100.0f);
	s->state1 = s->state2 = 0.0f;
}
static inline float pd_svf_bp(PdSvf* s, float in) {
	float hp = (in - s->r * s->state1 - s->g * s->state1 - s->state2) * s->h;
	float bp = s->g * hp + s->state1;
	s->state1 = s->g * hp + bp;
	float lp = s->g * bp + s->state2;
	s->state2 = s->g * bp + lp;
	return bp;
}
static inline float pd_svf_lp(PdSvf* s, float in) {
	float hp = (in - s->r * s->state1 - s->g * s->state1 - s->state2) * s->h;
	float bp = s->g * hp + s->state1;
	s->state1 = s->g * hp + bp;
	float lp = s->g * bp + s->state2;
	s->state2 = s->g * bp + lp;
	return lp;
}
static inline void pd_svf_bp_lp(PdSvf* s, float in, float* outBp, float* outLp) {
	float hp = (in - s->r * s->state1 - s->g * s->state1 - s->state2) * s->h;
	float bp = s->g * hp + s->state1;
	s->state1 = s->g * hp + bp;
	float lp = s->g * bp + s->state2;
	s->state2 = s->g * bp + lp;
	*outBp = bp;
	*outLp = lp;
}
static void pd_svf_bp_block(PdSvf* s, const float* in, float* out, uint32_t size) {
	float state1 = s->state1;
	float state2 = s->state2;
	while (size--) {
		float hp = (*in - s->r * state1 - s->g * state1 - state2) * s->h;
		float bp = s->g * hp + state1;
		state1 = s->g * hp + bp;
		float lp = s->g * bp + state2;
		state2 = s->g * bp + lp;
		*out++ = bp;
		++in;
	}
	s->state1 = state1;
	s->state2 = state2;
}
static void pd_svf_hp_block(PdSvf* s, const float* in, float* out, uint32_t size) {
	float state1 = s->state1;
	float state2 = s->state2;
	while (size--) {
		float hp = (*in - s->r * state1 - s->g * state1 - state2) * s->h;
		float bp = s->g * hp + state1;
		state1 = s->g * hp + bp;
		float lp = s->g * bp + state2;
		state2 = s->g * bp + lp;
		*out++ = hp;
		++in;
	}
	s->state1 = state1;
	s->state2 = state2;
}

/* --- PolyBLEP oscillator (plaits Oscillator, saw and square shapes) ------------------------------------------ */

#define PD_OSC_MAX_FREQUENCY 0.25f
#define PD_OSC_MIN_FREQUENCY 0.000001f

static inline float pd_this_blep_sample(float t) {
	return 0.5f * t * t;
}
static inline float pd_next_blep_sample(float t) {
	t = 1.0f - t;
	return -0.5f * t * t;
}

static void pd_osc_init(PdOscillator* o) {
	o->phase = 0.5f;
	o->nextSample = 0.0f;
	o->high = 1;
	o->frequency = 0.001f;
	o->pw = 0.5f;
}

static void pd_osc_render_saw(PdOscillator* o, float frequency, float pw, float* out, uint32_t size) {
	PD_CONSTRAIN(frequency, PD_OSC_MIN_FREQUENCY, PD_OSC_MAX_FREQUENCY);
	PD_CONSTRAIN(pw, frequency * 2.0f, 1.0f - 2.0f * frequency);

	PdInterpolator fm, pwm;
	pd_interp_init(&fm, &o->frequency, frequency, size);
	pd_interp_init(&pwm, &o->pw, pw, size);

	float next_sample = o->nextSample;

	while (size--) {
		float this_sample = next_sample;
		next_sample = 0.0f;

		const float f = pd_interp_next(&fm);
		(void)pd_interp_next(&pwm);
		o->phase += f;

		if (o->phase >= 1.0f) {
			o->phase -= 1.0f;
			float t = o->phase / f;
			this_sample -= pd_this_blep_sample(t);
			next_sample -= pd_next_blep_sample(t);
		}
		next_sample += o->phase;
		*out++ = 2.0f * this_sample - 1.0f;
	}
	o->nextSample = next_sample;
	pd_interp_done(&pwm);
	pd_interp_done(&fm);
}

static void pd_osc_render_square(PdOscillator* o, float frequency, float pw, float* out, uint32_t size) {
	PD_CONSTRAIN(frequency, PD_OSC_MIN_FREQUENCY, PD_OSC_MAX_FREQUENCY);
	PD_CONSTRAIN(pw, frequency * 2.0f, 1.0f - 2.0f * frequency);

	PdInterpolator fm, pwm;
	pd_interp_init(&fm, &o->frequency, frequency, size);
	pd_interp_init(&pwm, &o->pw, pw, size);

	float next_sample = o->nextSample;

	while (size--) {
		float this_sample = next_sample;
		next_sample = 0.0f;

		const float f = pd_interp_next(&fm);
		const float w = pd_interp_next(&pwm);
		o->phase += f;

		if (o->high ^ (o->phase >= w)) {
			float t = (o->phase - w) / f;
			this_sample += pd_this_blep_sample(t);
			next_sample += pd_next_blep_sample(t);
			o->high = o->phase >= w;
		}
		if (o->phase >= 1.0f) {
			o->phase -= 1.0f;
			float t = o->phase / f;
			this_sample -= pd_this_blep_sample(t);
			next_sample -= pd_next_blep_sample(t);
			o->high = 0;
		}
		next_sample += o->phase < w ? 0.0f : 1.0f;
		*out++ = 2.0f * this_sample - 1.0f;
	}
	o->nextSample = next_sample;
	pd_interp_done(&pwm);
	pd_interp_done(&fm);
}

/* --- Overdrive (plaits/dsp/fx/overdrive.h) ------------------------------------------------------------------- */

static void pd_overdrive_init(PdOverdrive* d) {
	d->preGain = 0.0f;
	d->postGain = 0.0f;
}

static void pd_overdrive_process(PdOverdrive* d, float drive, float* in_out, uint32_t size) {
	const float drive_2 = drive * drive;
	const float pre_gain_a = drive * 0.5f;
	const float pre_gain_b = drive_2 * drive_2 * drive * 24.0f;
	const float pre_gain = pre_gain_a + (pre_gain_b - pre_gain_a) * drive_2;
	const float drive_squashed = drive * (2.0f - drive);
	const float post_gain = 1.0f / pd_soft_clip(0.33f + drive_squashed * (pre_gain - 0.33f));

	PdInterpolator pre_gain_modulation, post_gain_modulation;
	pd_interp_init(&pre_gain_modulation, &d->preGain, pre_gain, size);
	pd_interp_init(&post_gain_modulation, &d->postGain, post_gain, size);

	while (size--) {
		float pre = pd_interp_next(&pre_gain_modulation) * *in_out;
		*in_out++ = pd_soft_clip(pre) * pd_interp_next(&post_gain_modulation);
	}
	pd_interp_done(&post_gain_modulation);
	pd_interp_done(&pre_gain_modulation);
}

/* --- 808 bass drum (plaits AnalogBassDrum) ------------------------------------------------------------------- */

static void pd_analog_kick_init(PdAnalogBassDrum* k) {
	k->pulseRemainingSamples = 0;
	k->fmPulseRemainingSamples = 0;
	k->pulse = 0.0f;
	k->pulseHeight = 0.0f;
	k->pulseLp = 0.0f;
	k->fmPulseLp = 0.0f;
	k->retrigPulse = 0.0f;
	k->lpOut = 0.0f;
	k->toneLp = 0.0f;
	pd_svf_init(&k->resonator);
	pd_overdrive_init(&k->overdrive);
}

static inline float pd_diode(float x) {
	if (x >= 0.0f) {
		return x;
	}
	x *= 2.0f;
	return 0.7f * x / (1.0f + pd_fabs(x));
}

static void pd_analog_kick_render(const DelugePluginHostApi* api, PdAnalogBassDrum* k, int32_t trigger, float accent,
                                  float f0, float tone, float decay, float attack_fm_amount, float self_fm_amount,
                                  float* out, uint32_t size) {
	const int32_t kTriggerPulseDuration = (int32_t)(1.0e-3f * SR);
	const int32_t kFMPulseDuration = (int32_t)(6.0e-3f * SR);
	const float kPulseDecayTime = 0.2e-3f * SR;
	const float kPulseFilterTime = 0.1e-3f * SR;
	const float kRetrigPulseDuration = 0.05f * SR;

	const float scale = 0.001f / f0;
	const float q = 1500.0f * pd_semitones_to_ratio(api, decay * 80.0f);
	const float tone_f = pd_min(4.0f * f0 * pd_semitones_to_ratio(api, tone * 108.0f), 1.0f);
	const float exciter_leak = 0.08f * (tone + 0.25f);

	if (trigger) {
		k->pulseRemainingSamples = kTriggerPulseDuration;
		k->fmPulseRemainingSamples = kFMPulseDuration;
		k->pulseHeight = 3.0f + 7.0f * accent;
		k->lpOut = 0.0f;
	}

	while (size--) {
		/* Q39 / Q40 */
		float pulse = 0.0f;
		if (k->pulseRemainingSamples) {
			--k->pulseRemainingSamples;
			pulse = k->pulseRemainingSamples ? k->pulseHeight : k->pulseHeight - 1.0f;
			k->pulse = pulse;
		}
		else {
			k->pulse *= 1.0f - 1.0f / kPulseDecayTime;
			pulse = k->pulse;
		}

		/* C40 / R163 / R162 / D83 */
		PD_ONE_POLE(k->pulseLp, pulse, 1.0f / kPulseFilterTime);
		pulse = pd_diode((pulse - k->pulseLp) + pulse * 0.044f);

		/* Q41 / Q42 */
		float fm_pulse = 0.0f;
		if (k->fmPulseRemainingSamples) {
			--k->fmPulseRemainingSamples;
			fm_pulse = 1.0f;
			/* C39 / C52 */
			k->retrigPulse = k->fmPulseRemainingSamples ? 0.0f : -0.8f;
		}
		else {
			/* C39 / R161 */
			k->retrigPulse *= 1.0f - 1.0f / kRetrigPulseDuration;
		}
		PD_ONE_POLE(k->fmPulseLp, fm_pulse, 1.0f / kPulseFilterTime);

		/* Q43 and R170 leakage */
		float punch = 0.7f + pd_diode(10.0f * k->lpOut - 1.0f);

		/* Q43 / R165 */
		float attack_fm = k->fmPulseLp * 1.7f * attack_fm_amount;
		float self_fm = punch * 0.08f * self_fm_amount;
		float f = f0 * (1.0f + attack_fm + self_fm);
		PD_CONSTRAIN(f, 0.0f, 0.4f);

		float resonator_out;
		pd_svf_set_g_q(&k->resonator, pd_tan_dirty(f), 1.0f + q * f);
		pd_svf_bp_lp(&k->resonator, (pulse - k->retrigPulse * 0.2f) * scale, &resonator_out, &k->lpOut);

		PD_ONE_POLE(k->toneLp, pulse * exciter_leak + resonator_out, tone_f);

		*out++ = k->toneLp;
	}
}

/* --- 808 snare drum (plaits AnalogSnareDrum) ----------------------------------------------------------------- */

static void pd_analog_snare_init(PdAnalogSnareDrum* s) {
	s->pulseRemainingSamples = 0;
	s->pulse = 0.0f;
	s->pulseHeight = 0.0f;
	s->pulseLp = 0.0f;
	s->noiseEnvelope = 0.0f;
	for (int32_t i = 0; i < PD_ANALOG_SNARE_NUM_MODES; ++i) {
		pd_svf_init(&s->resonator[i]);
	}
	pd_svf_init(&s->noiseFilter);
}

static inline float pd_analog_snare_mode_frequency(int32_t i) {
	switch (i) {
	case 0:
		return 1.00f;
	case 1:
		return 2.00f;
	case 2:
		return 3.18f;
	case 3:
		return 4.16f;
	default:
		return 5.62f;
	}
}

static void pd_analog_snare_render(const DelugePluginHostApi* api, PlaitsDrumsVoice* v, PdAnalogSnareDrum* s,
                                   int32_t trigger, float accent, float f0, float tone, float decay, float snappy,
                                   float* out, uint32_t size) {
	const float decay_xt = decay * (1.0f + decay * (decay - 1.0f));
	const int32_t kTriggerPulseDuration = (int32_t)(1.0e-3f * SR);
	const float kPulseDecayTime = 0.1e-3f * SR;
	const float q = 2000.0f * pd_semitones_to_ratio(api, decay_xt * 84.0f);
	const float noise_envelope_decay = 1.0f - 0.0017f * pd_semitones_to_ratio(api, -decay * (50.0f + snappy * 10.0f));
	const float exciter_leak = snappy * (2.0f - snappy) * 0.1f;

	snappy = snappy * 1.1f - 0.05f;
	PD_CONSTRAIN(snappy, 0.0f, 1.0f);

	if (trigger) {
		s->pulseRemainingSamples = kTriggerPulseDuration;
		s->pulseHeight = 3.0f + 7.0f * accent;
		s->noiseEnvelope = 2.0f;
	}

	float f[PD_ANALOG_SNARE_NUM_MODES];
	float gain[PD_ANALOG_SNARE_NUM_MODES];

	for (int32_t i = 0; i < PD_ANALOG_SNARE_NUM_MODES; ++i) {
		f[i] = pd_min(f0 * pd_analog_snare_mode_frequency(i), 0.499f);
		pd_svf_set_g_q(&s->resonator[i], pd_tan_fast(f[i]), 1.0f + f[i] * (i == 0 ? q : q * 0.25f));
	}

	if (tone < 0.666667f) {
		/* 808-style (2 modes) */
		tone *= 1.5f;
		gain[0] = 1.5f + (1.0f - tone) * (1.0f - tone) * 4.5f;
		gain[1] = 2.0f * tone + 0.15f;
		for (int32_t i = 2; i < PD_ANALOG_SNARE_NUM_MODES; ++i) {
			gain[i] = 0.0f;
		}
	}
	else {
		/* What the 808 could have been if there were extra modes! */
		tone = (tone - 0.666667f) * 3.0f;
		gain[0] = 1.5f - tone * 0.5f;
		gain[1] = 2.15f - tone * 0.7f;
		for (int32_t i = 2; i < PD_ANALOG_SNARE_NUM_MODES; ++i) {
			gain[i] = tone;
			tone *= tone;
		}
	}

	float f_noise = f0 * 16.0f;
	PD_CONSTRAIN(f_noise, 0.0f, 0.499f);
	pd_svf_set_g_q(&s->noiseFilter, pd_tan_fast(f_noise), 1.0f + f_noise * 1.5f);

	while (size--) {
		/* Q45 / Q46 */
		float pulse = 0.0f;
		if (s->pulseRemainingSamples) {
			--s->pulseRemainingSamples;
			pulse = s->pulseRemainingSamples ? s->pulseHeight : s->pulseHeight - 1.0f;
			s->pulse = pulse;
		}
		else {
			s->pulse *= 1.0f - 1.0f / kPulseDecayTime;
			pulse = s->pulse;
		}

		/* R189 / C57 / R190 + C58 / C59 / R197 / R196 / IC14 */
		PD_ONE_POLE(s->pulseLp, pulse, 0.75f);

		float shell = 0.0f;
		for (int32_t i = 0; i < PD_ANALOG_SNARE_NUM_MODES; ++i) {
			float excitation = i == 0 ? (pulse - s->pulseLp) + 0.006f * pulse : 0.026f * pulse;
			shell += gain[i] * (pd_svf_bp(&s->resonator[i], excitation) + excitation * exciter_leak);
		}
		shell = pd_soft_clip(shell);

		/* C56 / R194 / Q48 / C54 / R188 / D54 */
		float noise = 2.0f * pd_random_float(v) - 1.0f;
		if (noise < 0.0f) {
			noise = 0.0f;
		}
		s->noiseEnvelope *= noise_envelope_decay;
		noise *= s->noiseEnvelope * snappy * 2.0f;

		/* C66 / R201 / C67 / R202 / R203 / Q49 */
		noise = pd_svf_bp(&s->noiseFilter, noise);

		/* IC13 */
		*out++ = noise + shell * (1.0f - snappy);
	}
}

/* --- hi-hats (plaits HiHat<SquareNoise, SwingVCA, true, false> and <RingModNoise, LinearVCA, false, true>) --- */

static inline float pd_square_noise_ratio(int32_t i) {
	/* Nominal f0: 414 Hz */
	switch (i) {
	case 0:
		return 1.0f;
	case 1:
		return 1.304f;
	case 2:
		return 1.466f;
	case 3:
		return 1.787f;
	case 4:
		return 1.932f;
	default:
		return 2.536f;
	}
}

static void pd_square_noise_render(uint32_t* phase_state, float f0, float* out, uint32_t size) {
	uint32_t increment[6];
	uint32_t phase[6];
	for (int32_t i = 0; i < 6; ++i) {
		float f = f0 * pd_square_noise_ratio(i);
		if (f >= 0.499f) {
			f = 0.499f;
		}
		increment[i] = (uint32_t)(f * 4294967296.0f);
		phase[i] = phase_state[i];
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
		*out++ = 0.33f * (float)noise - 1.0f;
	}

	for (int32_t i = 0; i < 6; ++i) {
		phase_state[i] = phase[i];
	}
}

static void pd_ring_mod_pair(PdOscillator* osc, float fa, float fb, float* temp_1, float* temp_2, float* out,
                             uint32_t size) {
	pd_osc_render_square(&osc[0], fa, 0.5f, temp_1, size);
	pd_osc_render_saw(&osc[1], fb, 0.5f, temp_2, size);
	while (size--) {
		*out++ += *temp_1++ * *temp_2++;
	}
}

/* Three ring-modulated square x saw pairs - the "metallic" noise of Plaits' second hi-hat (its AUX output). */
static void pd_ring_mod_noise_render(PdOscillator* osc, float f0, float* out, float* temp_1, float* temp_2,
                                     uint32_t size) {
	/* Plaits wrote this with f0 in cycles-per-sample at 48 kHz; evaluate it at that scale so the pair frequencies
	 * (and therefore the ring-mod partials) are the same at the Deluge's 44.1 kHz. */
	const float f0_at_48k = f0 * (SR / 48000.0f);
	const float ratio = f0_at_48k / (0.01f + f0_at_48k);
	const float f1a = 200.0f / SR * ratio;
	const float f1b = 7530.0f / SR * ratio;
	const float f2a = 510.0f / SR * ratio;
	const float f2b = 8075.0f / SR * ratio;
	const float f3a = 730.0f / SR * ratio;
	const float f3b = 10500.0f / SR * ratio;

	for (uint32_t i = 0; i < size; ++i) {
		out[i] = 0.0f;
	}

	pd_ring_mod_pair(&osc[0], f1a, f1b, temp_1, temp_2, out, size);
	pd_ring_mod_pair(&osc[2], f2a, f2b, temp_1, temp_2, out, size);
	pd_ring_mod_pair(&osc[4], f3a, f3b, temp_1, temp_2, out, size);
}

static void pd_hi_hat_init(PdHiHat* h, int32_t ringMod) {
	h->envelope = 0.0f;
	h->noiseClock = 0.0f;
	h->noiseSample = 0.0f;
	if (ringMod) {
		for (int32_t i = 0; i < 6; ++i) {
			pd_osc_init(&h->metallic.ringMod[i]);
		}
	}
	else {
		for (int32_t i = 0; i < 6; ++i) {
			h->metallic.squarePhase[i] = 0;
		}
	}
	pd_svf_init(&h->noiseColorationSvf);
	pd_svf_init(&h->hpf);
}

static inline float pd_swing_vca(float s, float gain) {
	s *= s > 0.0f ? 4.0f : 0.1f;
	s = s / (1.0f + pd_fabs(s));
	return (s + 0.1f) * gain;
}

/* `ringMod` selects the second variant: ring-modulated noise, linear VCA, no filter resonance, two-stage envelope. */
static void pd_hi_hat_render(const DelugePluginHostApi* api, PlaitsDrumsVoice* v, PdHiHat* h, int32_t ringMod,
                             int32_t trigger, float accent, float f0, float tone, float decay, float noisiness,
                             float* out, float* temp_1, float* temp_2, uint32_t size) {
	const float envelope_decay = 1.0f - 0.003f * pd_semitones_to_ratio(api, -decay * 84.0f);
	const float cut_decay = 1.0f - 0.0025f * pd_semitones_to_ratio(api, -decay * 36.0f);

	if (trigger) {
		h->envelope = (1.5f + 0.5f * (1.0f - decay)) * (0.3f + 0.7f * accent);
	}

	/* Render the metallic noise. */
	if (ringMod) {
		pd_ring_mod_noise_render(h->metallic.ringMod, 2.0f * f0, out, temp_1, temp_2, size);
	}
	else {
		pd_square_noise_render(h->metallic.squarePhase, 2.0f * f0, out, size);
	}

	/* Apply BPF on the metallic noise. */
	float cutoff = 150.0f / SR * pd_semitones_to_ratio(api, tone * 72.0f);
	PD_CONSTRAIN(cutoff, 0.0f, 16000.0f / SR);
	pd_svf_set_g_q(&h->noiseColorationSvf, pd_tan_accurate(cutoff), ringMod ? 1.0f : 3.0f + 3.0f * tone);
	pd_svf_bp_block(&h->noiseColorationSvf, out, out, size);

	/* This is not at all part of the 808 circuit! But to add more variety, we add a variable amount of clocked
	 * noise to the output of the 6 schmitt trigger oscillators. */
	noisiness *= noisiness;
	float noise_f = f0 * (16.0f + 16.0f * (1.0f - noisiness));
	PD_CONSTRAIN(noise_f, 0.0f, 0.5f);

	for (uint32_t i = 0; i < size; ++i) {
		h->noiseClock += noise_f;
		if (h->noiseClock >= 1.0f) {
			h->noiseClock -= 1.0f;
			h->noiseSample = pd_random_float(v) - 0.5f;
		}
		out[i] += noisiness * (h->noiseSample - out[i]);
	}

	/* Apply VCA. */
	for (uint32_t i = 0; i < size; ++i) {
		h->envelope *= h->envelope > 0.5f || !ringMod ? envelope_decay : cut_decay;
		out[i] = ringMod ? out[i] * h->envelope : pd_swing_vca(out[i], h->envelope);
	}

	pd_svf_set_g_q(&h->hpf, pd_tan_accurate(cutoff), 0.5f);
	pd_svf_hp_block(&h->hpf, out, out, size);
}

/* --- 909 bass drum (plaits SyntheticBassDrum) ---------------------------------------------------------------- */

static void pd_synth_kick_init(PdSyntheticBassDrum* k) {
	k->phase = 0.0f;
	k->phaseNoise = 0.0f;
	k->f0 = 0.0f;
	k->fm = 0.0f;
	k->fmLp = 0.0f;
	k->bodyEnvLp = 0.0f;
	k->bodyEnv = 0.0f;
	k->transientEnv = 0.0f;
	k->transientEnvLp = 0.0f;
	k->bodyEnvPulseWidth = 0;
	k->fmPulseWidth = 0;
	k->toneLp = 0.0f;

	/* SyntheticBassDrumClick */
	k->clickLp = 0.0f;
	k->clickHp = 0.0f;
	pd_svf_init(&k->clickFilter);
	pd_svf_set_g_q(&k->clickFilter, pd_tan_fast(5000.0f / SR), 2.0f);

	/* SyntheticBassDrumAttackNoise */
	k->noiseLp = 0.0f;
	k->noiseHp = 0.0f;
}

static inline float pd_synth_kick_click(PdSyntheticBassDrum* k, float in) {
	PD_SLOPE(k->clickLp, in, 0.5f, 0.1f);
	PD_ONE_POLE(k->clickHp, k->clickLp, 0.04f);
	return pd_svf_lp(&k->clickFilter, k->clickLp - k->clickHp);
}

static inline float pd_synth_kick_attack_noise(PlaitsDrumsVoice* v, PdSyntheticBassDrum* k) {
	float sample = pd_random_float(v);
	PD_ONE_POLE(k->noiseLp, sample, 0.05f);
	PD_ONE_POLE(k->noiseHp, k->noiseLp, 0.005f);
	return k->noiseLp - k->noiseHp;
}

static inline float pd_synth_kick_distorted_sine(const DelugePluginHostApi* api, float phase, float phase_noise,
                                                 float dirtiness) {
	phase += phase_noise * dirtiness;
	int32_t phase_integral = (int32_t)phase;
	float phase_fractional = phase - (float)phase_integral;
	phase = phase_fractional;
	float triangle = (phase < 0.5f ? phase : 1.0f - phase) * 4.0f - 1.0f;
	float sine = 2.0f * triangle / (1.0f + pd_fabs(triangle));
	float clean_sine = pd_sine(api, phase + 0.75f);
	return sine + (1.0f - dirtiness) * (clean_sine - sine);
}

static inline float pd_transistor_vca(float s, float gain) {
	s = (s - 0.6f) * gain;
	return 3.0f * s / (2.0f + pd_fabs(s)) + gain * 0.3f;
}

static void pd_synth_kick_render(const DelugePluginHostApi* api, PlaitsDrumsVoice* v, PdSyntheticBassDrum* k,
                                 int32_t trigger, float accent, float f0, float tone, float decay, float dirtiness,
                                 float fm_envelope_amount, float fm_envelope_decay, float* out, uint32_t size) {
	decay *= decay;
	fm_envelope_decay *= fm_envelope_decay;

	PdInterpolator f0_mod;
	pd_interp_init(&f0_mod, &k->f0, f0, size);

	dirtiness *= pd_max(1.0f - 8.0f * f0, 0.0f);

	const float fm_decay = 1.0f - 1.0f / (0.008f * (1.0f + fm_envelope_decay * 4.0f) * SR);

	const float body_env_decay = 1.0f - 1.0f / (0.02f * SR) * pd_semitones_to_ratio(api, -decay * 60.0f);
	const float transient_env_decay = 1.0f - 1.0f / (0.005f * SR);
	const float tone_f = pd_min(4.0f * f0 * pd_semitones_to_ratio(api, tone * 108.0f), 1.0f);
	const float transient_level = tone;

	if (trigger) {
		k->fm = 1.0f;
		k->bodyEnv = k->transientEnv = 0.3f + 0.7f * accent;
		k->bodyEnvPulseWidth = (int32_t)(SR * 0.001f);
		k->fmPulseWidth = (int32_t)(SR * 0.0013f);
	}

	while (size--) {
		PD_ONE_POLE(k->phaseNoise, pd_random_float(v) - 0.5f, 0.002f);

		float mix = 0.0f;

		if (k->fmPulseWidth) {
			--k->fmPulseWidth;
			k->phase = 0.25f;
		}
		else {
			k->fm *= fm_decay;
			float fm = 1.0f + fm_envelope_amount * 3.5f * k->fmLp;
			k->phase += pd_min(pd_interp_next(&f0_mod) * fm, 0.5f);
			if (k->phase >= 1.0f) {
				k->phase -= 1.0f;
			}
		}

		if (k->bodyEnvPulseWidth) {
			--k->bodyEnvPulseWidth;
		}
		else {
			k->bodyEnv *= body_env_decay;
			k->transientEnv *= transient_env_decay;
		}

		const float envelope_lp_f = 0.1f;
		PD_ONE_POLE(k->bodyEnvLp, k->bodyEnv, envelope_lp_f);
		PD_ONE_POLE(k->transientEnvLp, k->transientEnv, envelope_lp_f);
		PD_ONE_POLE(k->fmLp, k->fm, envelope_lp_f);

		float body = pd_synth_kick_distorted_sine(api, k->phase, k->phaseNoise, dirtiness);
		float click = pd_synth_kick_click(k, k->bodyEnvPulseWidth ? 0.0f : 1.0f);
		float transient = click + pd_synth_kick_attack_noise(v, k);

		mix -= pd_transistor_vca(body, k->bodyEnvLp);
		mix -= transient * k->transientEnvLp * transient_level;

		PD_ONE_POLE(k->toneLp, mix, tone_f);
		*out++ = k->toneLp;
	}
	pd_interp_done(&f0_mod);
}

/* --- 909 snare drum (plaits SyntheticSnareDrum) -------------------------------------------------------------- */

static void pd_synth_snare_init(PdSyntheticSnareDrum* s) {
	s->phase[0] = 0.0f;
	s->phase[1] = 0.0f;
	s->drumAmplitude = 0.0f;
	s->snareAmplitude = 0.0f;
	s->fm = 0.0f;
	s->holdCounter = 0;
	pd_onepole_init(&s->drumLp);
	pd_onepole_init(&s->snareHp);
	pd_svf_init(&s->snareLp);
}

static inline float pd_synth_snare_distorted_sine(float phase) {
	float triangle = (phase < 0.5f ? phase : 1.0f - phase) * 4.0f - 1.3f;
	return 2.0f * triangle / (1.0f + pd_fabs(triangle));
}

static void pd_synth_snare_render(const DelugePluginHostApi* api, PlaitsDrumsVoice* v, PdSyntheticSnareDrum* s,
                                  int32_t trigger, float accent, float f0, float fm_amount, float decay, float snappy,
                                  float* out, uint32_t size) {
	const float decay_xt = decay * (1.0f + decay * (decay - 1.0f));
	fm_amount *= fm_amount;
	const float drum_decay =
	    1.0f - 1.0f / (0.015f * SR) * pd_semitones_to_ratio(api, -decay_xt * 72.0f - fm_amount * 12.0f + snappy * 7.0f);
	const float snare_decay = 1.0f - 1.0f / (0.01f * SR) * pd_semitones_to_ratio(api, -decay * 60.0f - snappy * 7.0f);
	const float fm_decay = 1.0f - 1.0f / (0.007f * SR);

	snappy = snappy * 1.1f - 0.05f;
	PD_CONSTRAIN(snappy, 0.0f, 1.0f);

	const float drum_level = pd_sqrt(1.0f - snappy);
	const float snare_level = pd_sqrt(snappy);

	const float snare_f_min = pd_min(10.0f * f0, 0.5f);
	const float snare_f_max = pd_min(35.0f * f0, 0.5f);

	pd_onepole_set_g(&s->snareHp, pd_tan_fast(snare_f_min));
	pd_svf_set_g_q(&s->snareLp, pd_tan_fast(snare_f_max), 0.5f + 2.0f * snappy);
	pd_onepole_set_g(&s->drumLp, pd_tan_fast(3.0f * f0));

	if (trigger) {
		s->snareAmplitude = s->drumAmplitude = 0.3f + 0.7f * accent;
		s->fm = 1.0f;
		s->phase[0] = s->phase[1] = 0.0f;
		s->holdCounter = (int32_t)((0.04f + decay * 0.03f) * SR);
	}

	while (size--) {
		/* Compute all D envelopes. The envelope for the drum has a very long tail. The envelope for the snare has
		 * a "hold" stage which lasts between 40 and 70 ms */
		s->drumAmplitude *= (s->drumAmplitude > 0.03f || !(size & 1)) ? drum_decay : 1.0f;
		if (s->holdCounter) {
			--s->holdCounter;
		}
		else {
			s->snareAmplitude *= snare_decay;
		}
		s->fm *= fm_decay;

		/* The 909 circuit has a funny kind of oscillator coupling - the signal leaving Q40's collector and
		 * resetting all oscillators allow some intermodulation. */
		float reset_noise = 0.0f;
		float reset_noise_amount = (0.125f - f0) * 8.0f;
		PD_CONSTRAIN(reset_noise_amount, 0.0f, 1.0f);
		reset_noise_amount *= reset_noise_amount;
		reset_noise_amount *= fm_amount;
		reset_noise += s->phase[0] > 0.5f ? -1.0f : 1.0f;
		reset_noise += s->phase[1] > 0.5f ? -1.0f : 1.0f;
		reset_noise *= reset_noise_amount * 0.025f;

		float f = f0 * (1.0f + fm_amount * (4.0f * s->fm));
		s->phase[0] += f;
		s->phase[1] += f * 1.47f;
		if (reset_noise_amount > 0.1f) {
			if (s->phase[0] >= 1.0f + reset_noise) {
				s->phase[0] = 1.0f - s->phase[0];
			}
			if (s->phase[1] >= 1.0f + reset_noise) {
				s->phase[1] = 1.0f - s->phase[1];
			}
		}
		else {
			if (s->phase[0] >= 1.0f) {
				s->phase[0] -= 1.0f;
			}
			if (s->phase[1] >= 1.0f) {
				s->phase[1] -= 1.0f;
			}
		}

		float drum = -0.1f;
		drum += pd_synth_snare_distorted_sine(s->phase[0]) * 0.60f;
		drum += pd_synth_snare_distorted_sine(s->phase[1]) * 0.25f;
		drum *= s->drumAmplitude * drum_level;
		drum = pd_onepole_lp(&s->drumLp, drum);

		float noise = pd_random_float(v);
		float snare = pd_svf_lp(&s->snareLp, noise);
		snare = pd_onepole_hp(&s->snareHp, snare);
		snare = (snare + 0.1f) * (s->snareAmplitude + s->fm) * snare_level;

		*out++ = snare + drum; /* It's a snare, it's a drum, it's a snare drum. */
	}
}

/* --- the plugin ---------------------------------------------------------------------------------------------- */

/* Pitch scale so a model played at the Deluge's default note (C3 in a kit row) lands in its classic register:
 * kicks two octaves down, snares one octave down, hi-hats as-is. */
static inline float pd_model_pitch_scale(uint32_t model) {
	switch (model) {
	case PLAITS_DRUMS_MODEL_808_KICK:
	case PLAITS_DRUMS_MODEL_909_KICK:
		return 0.25f;
	case PLAITS_DRUMS_MODEL_808_SNARE:
	case PLAITS_DRUMS_MODEL_909_SNARE:
		return 0.5f;
	default:
		return 1.0f;
	}
}

static inline float pd_unit_from_q31(int32_t x) {
	return pd_clamp((float)x * (1.0f / 2147483648.0f), 0.0f, 1.0f);
}

void plaits_drums_init(void* state, uint32_t model, uint32_t seed) {
	PlaitsDrumsVoice* v = (PlaitsDrumsVoice*)state;
	if (model >= PLAITS_DRUMS_NUM_MODELS) {
		model = PLAITS_DRUMS_MODEL_808_KICK;
	}
	v->model = model;
	v->triggerPending = 0;
	v->silentBlocks = 0;
	v->accent = 0.0f;
	v->rng = seed;
	switch (model) {
	case PLAITS_DRUMS_MODEL_808_KICK:
		pd_analog_kick_init(&v->m.analogKick);
		break;
	case PLAITS_DRUMS_MODEL_808_SNARE:
		pd_analog_snare_init(&v->m.analogSnare);
		break;
	case PLAITS_DRUMS_MODEL_HI_HAT:
		pd_hi_hat_init(&v->m.hiHat, 0);
		break;
	case PLAITS_DRUMS_MODEL_909_KICK:
		pd_synth_kick_init(&v->m.synthKick);
		break;
	case PLAITS_DRUMS_MODEL_909_SNARE:
		pd_synth_snare_init(&v->m.synthSnare);
		break;
	default:
		pd_hi_hat_init(&v->m.hiHat, 1);
		break;
	}
}

void plaits_drums_trigger(void* state, int32_t accent) {
	PlaitsDrumsVoice* v = (PlaitsDrumsVoice*)state;
	v->accent = pd_unit_from_q31(accent);
	v->triggerPending = 1;
	v->silentBlocks = 0;
}

int32_t plaits_drums_render(const DelugePluginHostApi* api, void* state, const int32_t* macros, uint32_t phaseIncrement,
                            int32_t* out, uint32_t numSamples, void* scratch) {
	PlaitsDrumsVoice* v = (PlaitsDrumsVoice*)state;
	if (numSamples > PLAITS_DRUMS_MAX_BLOCK_SIZE) {
		numSamples = PLAITS_DRUMS_MAX_BLOCK_SIZE;
	}
	float* buf = (float*)scratch;
	float* temp_1 = buf + PLAITS_DRUMS_MAX_BLOCK_SIZE;
	float* temp_2 = temp_1 + PLAITS_DRUMS_MAX_BLOCK_SIZE;

	const int32_t trigger = v->triggerPending;
	v->triggerPending = 0;
	const float accent = v->accent;

	/* phaseIncrement is cycles-per-sample in q32, which is exactly the models' f0 unit. */
	const float f0 = (float)phaseIncrement * (1.0f / 4294967296.0f) * pd_model_pitch_scale(v->model);
	const float tone = pd_unit_from_q31(macros[PLAITS_DRUMS_MACRO_TONE]);
	const float decay = pd_unit_from_q31(macros[PLAITS_DRUMS_MACRO_DECAY]);
	const float harmonics = pd_unit_from_q31(macros[PLAITS_DRUMS_MACRO_SNAP]);

	/* Parameter mapping follows plaits/dsp/engine/{bass_drum,snare_drum,hi_hat}_engine.cc */
	switch (v->model) {
	case PLAITS_DRUMS_MODEL_808_KICK: {
		const float attack_fm_amount = pd_min(harmonics * 4.0f, 1.0f);
		const float self_fm_amount = pd_clamp(harmonics * 4.0f - 1.0f, 0.0f, 1.0f);
		const float drive = pd_max(harmonics * 2.0f - 1.0f, 0.0f) * pd_max(1.0f - 16.0f * f0, 0.0f);
		pd_analog_kick_render(api, &v->m.analogKick, trigger, accent, f0, tone, decay, attack_fm_amount, self_fm_amount,
		                      buf, numSamples);
		pd_overdrive_process(&v->m.analogKick.overdrive, 0.5f + 0.5f * drive, buf, numSamples);
		break;
	}
	case PLAITS_DRUMS_MODEL_808_SNARE:
		pd_analog_snare_render(api, v, &v->m.analogSnare, trigger, accent, f0, tone, decay, harmonics, buf, numSamples);
		break;
	case PLAITS_DRUMS_MODEL_HI_HAT:
		pd_hi_hat_render(api, v, &v->m.hiHat, 0, trigger, accent, f0, tone, decay, harmonics, buf, temp_1, temp_2,
		                 numSamples);
		break;
	case PLAITS_DRUMS_MODEL_909_KICK:
		pd_synth_kick_render(api, v, &v->m.synthKick, trigger, accent, f0, tone, decay, 0.4f - 0.25f * decay * decay,
		                     pd_min(harmonics * 2.0f, 1.0f), pd_max(harmonics * 2.0f - 1.0f, 0.0f), buf, numSamples);
		break;
	case PLAITS_DRUMS_MODEL_909_SNARE:
		pd_synth_snare_render(api, v, &v->m.synthSnare, trigger, accent, f0, tone, decay, harmonics, buf, numSamples);
		break;
	default:
		pd_hi_hat_render(api, v, &v->m.hiHat, 1, trigger, accent, f0, tone, decay, harmonics, buf, temp_1, temp_2,
		                 numSamples);
		break;
	}

	/* Silence detection on the float signal, then the q31 conversion the host used to do. */
	float peak = 0.0f;
	for (uint32_t i = 0; i < numSamples; i++) {
		const float clamped = pd_clamp(buf[i], -1.0f, 1.0f);
		peak = pd_max(peak, pd_fabs(buf[i]));
		out[i] = (int32_t)(clamped * 2147483520.0f);
	}

	if (trigger) {
		return 1;
	}
	if (peak >= PD_SILENCE_THRESHOLD) {
		v->silentBlocks = 0;
		return 1;
	}
	if (v->silentBlocks < PD_SILENT_BLOCKS_TO_RELEASE) {
		v->silentBlocks++;
		return 1;
	}
	return 0;
}
