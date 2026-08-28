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
 * Tape-style saturation: pre-emphasis (1 - 0.5z^-1) into a slightly biased antialiased tanh, then the exact
 * inverse de-emphasis and a DC blocker. HF gets compressed harder than lows, so drive softens the top end and
 * adds mostly 2nd/3rd harmonic. Small-signal gain stays at unity; drive only lowers the ceiling the peaks get
 * squashed into (6dB per eighth of the knob).
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

void tape_saturation_reset(void* state) {
	TapeSaturationState* s = (TapeSaturationState*)state;
	for (int32_t ch = 0; ch < 2; ch++) {
		s->preEmphLast[ch] = 0;
		s->deEmphLast[ch] = 0;
		s->dcBlock[ch] = 0;
		s->tanhWorkingValue[ch] = 2147483648u;
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

	/* Drive is fineGain * 2^driveShift, continuous across the whole knob. The context's level shift anchors the
	 * knob to that insertion point's real internal levels, which sit well below q31 full scale; 2 of its steps
	 * undo the /4 inherent in the q30 fine-gain multiply. */
	uint32_t driveBase = context->levelShift;
	uint32_t saturationAmount = (positive >> 29) + driveBase;
	int32_t fineGain = (int32_t)((1u << 30) + ((positive & 0x1FFFFFFF) << 1)); /* q30, [1.0, 2.0) */

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
	int32_t biasInput = (int32_t)(positive >> 5) >> saturationAmount;
	int32_t outOffset = api->tanhUnknown(biasInput, saturationAmount);

	for (uint32_t i = 0; i < numSamples; i++) {
		int32_t* channels[2] = {&buffer[i].l, &buffer[i].r};

		for (int32_t ch = 0; ch < 2; ch++) {
			int32_t x = *channels[ch];
			int32_t pre = (x >> 1) - (s->preEmphLast[ch] >> 2); /* halved for headroom; de-emphasis restores it */
			s->preEmphLast[ch] = x;
			int32_t driven = mulHigh(pre, fineGain);
			int32_t shaped =
			    api->tanhAntialiased(driven + biasInput, &s->tanhWorkingValue[ch], saturationAmount) - outOffset;
			shaped = mulHigh(shaped, makeupGain) << makeupShift;
			int32_t de = shaped + (s->deEmphLast[ch] >> 1);
			s->deEmphLast[ch] = de;
			int32_t y = de << 1;
			s->dcBlock[ch] += (y - s->dcBlock[ch]) >> 8;
			*channels[ch] = y - s->dcBlock[ch];
		}
	}
}
