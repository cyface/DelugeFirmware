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
 * Deluge plugin ABI, tier 1: a versioned C interface between the firmware (the host) and a DSP kernel
 * (the plugin). Plain C so a kernel can be compiled either straight into the firmware or, later, as a
 * standalone position-independent blob loaded from the SD card into SDRAM (see the #37 spike).
 *
 * Contract for a kernel written against this header:
 *   - No globals, no statics, no libc: all state lives in a host-owned block of `stateSize` bytes and
 *     everything else arrives through the argument list. This is what keeps a blob free of relocations.
 *   - Every call back into the firmware goes through the DelugePluginHostApi table. Nothing is imported.
 *   - Integer DSP only (q31 samples, host block size), no floating point in the interface.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when any struct or function signature below changes shape. */
#define DELUGE_PLUGIN_ABI_VERSION 1u

/* One stereo frame. Same layout as the firmware's StereoSample (asserted host-side), so a plugin
 * renders in place on the firmware's own buffer. */
typedef struct {
	int32_t l;
	int32_t r;
} DelugePluginStereoSample;

/* Services the host hands to a plugin. Function pointers so an SDRAM blob can call them without
 * symbol resolution; `version` lets a plugin refuse a host it does not understand. */
typedef struct {
	uint32_t version; /* DELUGE_PLUGIN_ABI_VERSION of the host */

	/* Antialiased tanh through the firmware's 2-D lookup table. `lastWorkingValue` is per-channel state the
	 * plugin keeps (initialise to 0x80000000); `saturationAmount` is the pre-shift in bits (drive). */
	int32_t (*tanhAntialiased)(int32_t input, uint32_t* lastWorkingValue, uint32_t saturationAmount);

	/* Static (non-antialiased) tanh through the small lookup table, same drive convention. */
	int32_t (*tanhUnknown)(int32_t input, uint32_t saturationAmount);
} DelugePluginHostApi;

/* Where in the signal chain an insert FX is running. Filled by the host per call. */
typedef struct {
	/* How far below q31 full scale the signal at this insertion point sits, in bits, used by plugins to
	 * anchor drive-style controls: a lone Sound ~9, an audio clip / kit ~8, the summed song master ~4. */
	uint32_t levelShift;
} DelugeFxContext;

/* Reset all state to "just switched on". `state` is the host-owned block for this instance. */
typedef void (*DelugeFxResetFn)(void* state);

/* Render `numSamples` stereo frames in place. `params` holds `numParams` raw q31 unpatched values as
 * stored by the firmware (INT32_MIN..INT32_MAX); the host decides whether the FX is enabled at all and
 * only calls render while it is. */
typedef void (*DelugeFxRenderFn)(const DelugePluginHostApi* api, void* state, const int32_t* params,
                                 const DelugeFxContext* context, DelugePluginStereoSample* buffer, uint32_t numSamples);

/* What the host needs to know about an insert-FX plugin. Built-in plugins get one of these in the
 * firmware's registry (plugin/host/builtin_fx.cpp); a loaded blob will describe itself the same way. */
typedef struct {
	uint32_t abiVersion; /* DELUGE_PLUGIN_ABI_VERSION the plugin was built against */
	const char* name;    /* short display name */
	uint32_t numParams;  /* entries the plugin reads from `params` */
	uint32_t stateSize;  /* bytes of state the host allocates per instance (4-byte aligned) */
	DelugeFxResetFn reset;
	DelugeFxRenderFn render;
} DelugeFxPlugin;

#ifdef __cplusplus
}
#endif
