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
 */

/*
 * Tape-style saturation: pre-emphasis (1 - 0.5z^-1) into a slightly biased antialiased tanh, then the exact inverse
 * de-emphasis and a DC blocker. HF gets compressed harder than lows, so drive softens the top end and adds mostly
 * 2nd/3rd harmonic. Small-signal gain stays at unity; drive only lowers the ceiling the peaks get squashed into
 * (6dB per eighth of the knob).
 *
 * Two stages borrowed from Airwindows ToTape9 (MIT), reformulated in fixed point (fork issue #40):
 *  - Slew-dependent averaging around the shaper: a 2-tap average is mixed in before and after the tanh in
 *    proportion to how fast the (drive-normalised) signal is moving, so fast transients come back darker while
 *    sustained material is untouched. Shift/add only.
 *  - Head bump: a leaky, cubic-self-limited integrator of the shaped signal through a band-pass biquad at 69 Hz,
 *    mixed back in by the HEAD_BUMP param. This is the low-end weight that makes tape sound like tape rather than
 *    a soft clipper. ToTape9 uses a pure integrator and two band-passes; that reaches 180 degrees above the bump
 *    and notches the low mids (-10dB at 170Hz at full mix), so this leaks the integrator at 120Hz and uses one
 *    band-pass: +7dB at 60-70Hz, still +2dB at 170Hz, -1.4dB at 300Hz.
 *
 * Plugin-contract kernel: no globals, no statics, no libc, firmware reached only through the host API table.
 * Compiles into the firmware directly and, unchanged, as a freestanding position-independent blob
 * (plugin/check_freestanding.sh proves the latter).
 */
#include "plugin/fx/tape_saturation.h"

/* (a * b) >> 32 on the 64-bit product - what the firmware's multiply_32x32_rshift32 (smmul) computes. */
static inline int32_t mulHigh(int32_t a, int32_t b) {
	return (int32_t)(((int64_t)a * b) >> 32);
}

/* a << shift, saturated to int32 (the firmware's lshiftAndSaturate, without the ssat instruction). */
static inline int32_t lshiftSaturate(int32_t a, uint32_t shift) {
	int32_t hi = (int32_t)0x7FFFFFFF >> shift;
	if (a > hi) {
		return 0x7FFFFFFF;
	}
	if (a < -hi - 1) {
		return (int32_t)0x80000000;
	}
	return a << shift;
}

static inline int32_t clamp64(int64_t v) {
	if (v > 0x7FFFFFFF) {
		return 0x7FFFFFFF;
	}
	if (v < -0x7FFFFFFF - 1) {
		return (int32_t)0x80000000;
	}
	return (int32_t)v;
}

/* floor(n / d) for a quotient known to fit in 32 bits. Cortex-A9 has no divide instruction, so a plain `/`
 * would import libgcc's __aeabi_uldivmod - not allowed in a freestanding blob. Once per block, so the
 * 64-step restoring loop is nothing. */
static uint32_t udiv64by32(uint64_t n, uint32_t d) {
	uint64_t r = 0;
	uint32_t q = 0;
	for (int32_t i = 63; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1u);
		q <<= 1;
		if (r >= d) {
			r -= d;
			q |= 1u;
		}
	}
	return q;
}

/* Band-pass biquad for the head bump at 44.1kHz: 68.75Hz, Q = 0.618, unity peak gain, a1 = 0. q30 coefficients
 * (b1 is < -1, hence q30 not q31). */
#define HB_A0 8441830
#define HB_B1 (-2130497777)
#define HB_B2 1056858164
/* Integrator: drive 0.1 (q32), leak 1 - e^(-2*pi*120/44100) (q32), cubic self-limit 0.0618 (q32). */
#define HB_DRIVE 429496730
#define HB_LEAK 72807327
#define HB_CUBIC 265428979
/* The head bump path runs 1.0 = 2^29 (two bits under the shaper's normalisation) for biquad headroom. */
#define HB_NORM_SHIFT 2

/* Transposed direct form II, coefficients q30, 64-bit products. */
static inline int32_t bandpass(int32_t x, int32_t* s, int32_t a0, int32_t b1, int32_t b2) {
	int32_t y = clamp64((((int64_t)a0 * x) >> 30) + s[0]);
	s[0] = clamp64(((-(int64_t)b1 * y) >> 30) + s[1]);
	s[1] = clamp64(((-(int64_t)a0 * x) - ((int64_t)b2 * y)) >> 30); /* a2 = -a0 */
	return y;
}

void tape_saturation_reset(void* state) {
	TapeSaturationState* s = (TapeSaturationState*)state;
	for (int32_t ch = 0; ch < 2; ch++) {
		s->preEmphLast[ch] = 0;
		s->deEmphLast[ch] = 0;
		s->dcBlock[ch] = 0;
		s->tanhWorkingValue[ch] = 2147483648u;
		s->slewLastIn[ch] = 0;
		s->slewLastDark[ch] = 0;
		s->slewLastOut[ch] = 0;
		s->headBump[ch] = 0;
		s->headBumpBp[ch][0] = s->headBumpBp[ch][1] = 0;
	}
}

/* The knob at its minimum is "off": bypassed entirely rather than a zero-drive pass through the shaper. */
int32_t tape_saturation_is_active(const int32_t* params) {
	return params[TAPE_SATURATION_PARAM_AMOUNT] != (int32_t)0x80000000;
}

void tape_saturation_render(const DelugePluginHostApi* api, void* state, const int32_t* params,
                            const DelugeFxContext* context, DelugePluginStereoSample* buffer, uint32_t numSamples) {
	TapeSaturationState* s = (TapeSaturationState*)state;

	uint32_t positive = (uint32_t)params[TAPE_SATURATION_PARAM_AMOUNT] + 2147483648u;

	/* Knob taper (fork #40 feedback: a straight 6dB-per-eighth ramp was already crunchy at 14%): the lower half of
	 * the knob spans ONE 6dB drive step, the upper half the remaining seven, so the subtle zone takes half the
	 * travel and the top still reaches the same drive. `drive` is in eighth-of-the-old-knob steps, q29. */
	uint32_t drive;
	if (positive < 2147483648u) {
		drive = positive >> 2;
	}
	else {
		uint32_t upper = positive - 2147483648u;
		drive = (1u << 29) + upper + (upper >> 1) + (upper >> 2); /* 1 + 7 * upper/2^31 steps, max 2^32 - 3 */
	}

	/* Drive is fineGain * 2^driveShift, continuous across the whole knob. The context's level shift anchors the
	 * knob to that insertion point's real internal levels, which sit well below q31 full scale; 2 of its steps
	 * undo the /4 inherent in the q30 fine-gain multiply. */
	uint32_t driveBase = context->levelShift;
	uint32_t saturationAmount = (drive >> 29) + driveBase;
	int32_t fineGain = (int32_t)((1u << 30) + ((drive & 0x1FFFFFFF) << 1)); /* q30, [1.0, 2.0) */

	/* Post-shape makeup. 1154084861100017408 = (8 / 1.998) * 2^58, with 1.998 the measured centre slope of tanH2d.
	 * Below the crossover (75% of the knob), gain is 8 / (tableSlope * fineGain) - unity small-signal gain, with
	 * drive lowering the ceiling peaks squash into. Above it the ceiling holds and small-signal gain rises 6dB per
	 * step instead (g = 2^(sat-crossover) * fineGain, exactly 1.0 at the boundary, so the knob sweep stays
	 * continuous): quiet sources get pushed up into the shaper rather than the ceiling dropping below their reach. */
	uint32_t crossover = driveBase + 6;
	int32_t makeupGain;
	int32_t makeupShift;
	if (saturationAmount >= crossover) {
		makeupGain = (int32_t)(1154084861100017408uLL >> 30);
		makeupShift = 4 + (int32_t)(saturationAmount - crossover);
	}
	else {
		makeupGain = (int32_t)udiv64by32(1154084861100017408uLL, (uint32_t)fineGain);
		makeupShift = 4;
	}

	/* A little input bias makes the shaper asymmetric for 2nd-harmonic warmth; the static output offset is
	 * subtracted straight back out and the DC blocker catches the signal-dependent remainder. */
	int32_t biasInput = (int32_t)(drive >> 5) >> saturationAmount;
	int32_t outOffset = api->tanhUnknown(biasInput, saturationAmount);

	/* Slew measure: |delta| of the shaper input in the shaper's own units (1.0 = the tanh table edge, i.e.
	 * driven << saturationAmount), times 0.25, clamped to 1.0. That is ToTape9's 0.12 * 2 at 44.1kHz, and it
	 * scales with drive on its own: the harder the tape is pushed, the more transients are darkened. */
	uint32_t slewShift = saturationAmount - 2; /* saturationAmount >= levelShift >= 4 */

	/* Head bump: the knob only sets the mix (0.5 * amount, q32), so it is linear in loudness; the integrator's
	 * drive and self-limit are fixed at ToTape9's proportions. */
	uint32_t bump = (uint32_t)params[TAPE_SATURATION_PARAM_HEAD_BUMP] + 2147483648u;
	int32_t bumpMix = (int32_t)(bump >> 1);
	uint32_t bumpShift = saturationAmount - HB_NORM_SHIFT;
	if (bump == 0) {
		/* Off: keep the LF path quiet so turning it up later starts from silence, not a stale resonance. */
		for (int32_t ch = 0; ch < 2; ch++) {
			s->headBump[ch] = 0;
			s->headBumpBp[ch][0] = s->headBumpBp[ch][1] = 0;
		}
	}

	for (uint32_t i = 0; i < numSamples; i++) {
		int32_t* channels[2] = {&buffer[i].l, &buffer[i].r};

		for (int32_t ch = 0; ch < 2; ch++) {
			int32_t x = *channels[ch];
			int32_t pre = (x >> 1) - (s->preEmphLast[ch] >> 2); /* halved for headroom; de-emphasis restores it */
			s->preEmphLast[ch] = x;
			int32_t driven = mulHigh(pre, fineGain);

			/* Slew-dependent 2-tap average into the shaper. */
			int32_t dark = (driven >> 1) + (s->slewLastIn[ch] >> 1);
			s->slewLastIn[ch] = driven;
			int64_t delta = (int64_t)driven - s->slewLastDark[ch];
			if (delta < 0) {
				delta = -delta;
			}
			delta <<= slewShift;
			int32_t w = delta > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)delta; /* q31 mix weight */
			s->slewLastDark[ch] = dark;
			driven += mulHigh(dark - driven, w) << 1;

			int32_t shaped =
			    api->tanhAntialiased(driven + biasInput, &s->tanhWorkingValue[ch], saturationAmount) - outOffset;

			/* ...and the same weight of averaging out of it. */
			int32_t darkOut = (shaped >> 1) + (s->slewLastOut[ch] >> 1);
			s->slewLastOut[ch] = shaped;
			shaped += mulHigh(darkOut - shaped, w) << 1;

			int32_t v = mulHigh(shaped, makeupGain) << makeupShift;

			if (bump != 0) {
				/* Integrate the shaped signal (normalised so 1.0 = 2^29), self-limit with a cubic pull-back, then
				 * band-pass twice and mix the result back in at the original scale. */
				int32_t n = lshiftSaturate(v, bumpShift);
				int32_t hb = s->headBump[ch];
				hb = clamp64((int64_t)hb + mulHigh(n, HB_DRIVE) - mulHigh(hb, HB_LEAK));
				int32_t hb2 = mulHigh(hb, hb) << (HB_NORM_SHIFT + 1); /* q29 * q29 -> q29 */
				int32_t hb3 = mulHigh(hb2, hb) << (HB_NORM_SHIFT + 1);
				hb = clamp64((int64_t)hb - mulHigh(hb3, HB_CUBIC));
				s->headBump[ch] = hb;
				int32_t bumpOut = bandpass(hb, s->headBumpBp[ch], HB_A0, HB_B1, HB_B2);
				v += mulHigh(bumpOut, bumpMix) >> bumpShift;
			}

			int32_t de = v + (s->deEmphLast[ch] >> 1);
			s->deEmphLast[ch] = de;
			int32_t y = de << 1;
			s->dcBlock[ch] += (y - s->dcBlock[ch]) >> 8;
			*channels[ch] = y - s->dcBlock[ch];
		}
	}
}
