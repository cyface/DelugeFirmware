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

/* Tape saturation insert FX as a tier-1 plugin kernel. See docs/dev/tape_saturation.md for the design and
 * the calibration behind the constants. */
#pragma once
#include "plugin/plugin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Raw q31 unpatched values: AMOUNT is the Tape knob (drive/character, minimum = off), HEAD_BUMP the amount of
 * low-frequency head bump mixed back in (minimum = none). */
enum {
	TAPE_SATURATION_PARAM_AMOUNT = 0,
	TAPE_SATURATION_PARAM_HEAD_BUMP = 1,
	TAPE_SATURATION_NUM_PARAMS = 2,
};

/* Host-owned per-instance state. Index 0 = left, 1 = right. */
typedef struct {
	int32_t preEmphLast[2];
	int32_t deEmphLast[2];
	int32_t dcBlock[2];
	uint32_t tanhWorkingValue[2];
	int32_t slewLastIn[2];    /* previous shaper input, for the 2-tap average */
	int32_t slewLastDark[2];  /* previous averaged input, for the slew measure */
	int32_t slewLastOut[2];   /* previous shaper output, for the post-shaper average */
	int32_t headBump[2];      /* leaky, cubic-limited integrator */
	int32_t headBumpBp[2][2]; /* band-pass biquad, transposed direct form II state */
} TapeSaturationState;

void tape_saturation_reset(void* state);
int32_t tape_saturation_is_active(const int32_t* params);
void tape_saturation_render(const DelugePluginHostApi* api, void* state, const int32_t* params,
                            const DelugeFxContext* context, DelugePluginStereoSample* buffer, uint32_t numSamples);

#ifdef __cplusplus
}
#endif
