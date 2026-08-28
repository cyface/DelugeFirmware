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
 *   - Samples, params and pitch cross the boundary as integers (q31 samples, host block size). A kernel may
 *     use single-precision float internally: both sides are built -mfloat-abi=hard -mfpu=neon, which makes the
 *     VFP calling convention part of this ABI, so the host API also offers a few float services.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when any struct or function signature below changes shape. */
#define DELUGE_PLUGIN_ABI_VERSION 3u

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

	/* 2^x in single precision, for the pitch-ratio maths of float kernels (libm is not linkable from a blob). */
	float (*exp2f)(float x);

	/* Sine lookup table for float kernels: DELUGE_PLUGIN_SINE_LUT_SIZE + 1 entries, entry i = sin(2*pi*i/SIZE),
	 * the extra entry a copy of the first so a linear interpolation at phase 1.0 reads no further. */
	const float* sineLut;
} DelugePluginHostApi;

#define DELUGE_PLUGIN_SINE_LUT_SIZE 512u

/* Where in the signal chain an insert FX is running. Filled by the host per call. */
typedef struct {
	/* How far below q31 full scale the signal at this insertion point sits, in bits, used by plugins to
	 * anchor drive-style controls: a lone Sound ~9, an audio clip / kit ~8, the summed song master ~4. */
	uint32_t levelShift;
} DelugeFxContext;

/* One parameter a plugin exposes. The host owns the storage (a slot in its shared unpatched param bank) and
 * generates the menu entry, automation entry, XML attribute and display names from this. */
typedef struct {
	const char* name;      /* display name, OLED (<= 14 chars) */
	const char* shortName; /* display name, 7-segment (<= 4 chars) */
	const char* fileName;  /* XML attribute; must be unique across every param the firmware knows */
	int32_t defaultValue;  /* raw q31, what a fresh preset gets */
} DelugeFxParamInfo;

/* Reset all state to "just switched on". `state` is the host-owned block for this instance. */
typedef void (*DelugeFxResetFn)(void* state);

/* Optional: is the FX doing anything at these param values? While it returns 0 the host bypasses it, so a plugin
 * that is "off" at its defaults costs nothing. NULL means always active. */
typedef int32_t (*DelugeFxIsActiveFn)(const int32_t* params);

/* Render `numSamples` stereo frames in place. `params` holds `numParams` raw q31 unpatched values as
 * stored by the firmware (INT32_MIN..INT32_MAX); the host decides whether the FX is enabled at all and
 * only calls render while it is. */
typedef void (*DelugeFxRenderFn)(const DelugePluginHostApi* api, void* state, const int32_t* params,
                                 const DelugeFxContext* context, DelugePluginStereoSample* buffer, uint32_t numSamples);

/* What the host needs to know about an insert-FX plugin. Built-in plugins get one of these in the
 * firmware's registry (plugin/host/builtin_fx.cpp); a loaded blob will describe itself the same way. */
typedef struct {
	uint32_t abiVersion;                /* DELUGE_PLUGIN_ABI_VERSION the plugin was built against */
	const char* name;                   /* short display name */
	uint32_t numParams;                 /* entries in `paramInfo`, and what `params` holds at render time */
	const DelugeFxParamInfo* paramInfo; /* one per param, in `params` order */
	uint32_t stateSize;                 /* bytes of state the host allocates per instance (4-byte aligned) */
	DelugeFxResetFn reset;
	DelugeFxIsActiveFn isActive; /* may be NULL */
	DelugeFxRenderFn render;
} DelugeFxPlugin;

/* ----------------------------------------------------------------------------------------------------------------
 * Source plugins: an oscillator type (OscType::DRUM today) whose per-voice DSP is a plugin kernel. The host owns
 * one state block per voice (per unison part), triggers it on note-on, and asks it for mono q31 blocks until the
 * kernel reports the voice has finished. The kernel offers a list of models (the Deluge shows them as a menu and
 * saves the chosen one by its fileName) and up to DELUGE_SOURCE_PLUGIN_MAX_MACROS macro controls that the host
 * routes from ordinary automatable params.
 */

#define DELUGE_SOURCE_PLUGIN_MAX_MACROS 3u

/* A display name in both of the Deluge's forms. */
typedef struct {
	const char* name;      /* OLED (<= 14 chars) */
	const char* shortName; /* 7-segment (<= 4 chars) */
} DelugePluginName;

typedef struct {
	DelugePluginName name;
	const char* fileName; /* XML value identifying this model; unique within the plugin */
	/* What each macro means for this model (a kick's third macro is Drive, a snare's Snappy). Unused macros NULL. */
	DelugePluginName macros[DELUGE_SOURCE_PLUGIN_MAX_MACROS];
} DelugeSourceModelInfo;

/* Reset a voice's state block to silence for `model` (an index below numModels). `seed` initialises whatever noise
 * source the kernel keeps in the voice, so two hits never share a noise pattern. Leaves the voice untriggered. */
typedef void (*DelugeSourceInitFn)(void* state, uint32_t model, uint32_t seed);

/* Arm a hit: the next render starts a new note at `accent` (q31, 0..INT32_MAX = 0..1, from velocity). Calling it
 * on a still-ringing voice restarts the excitation without clearing the resonators, like hitting a real drum. */
typedef void (*DelugeSourceTriggerFn)(void* state, int32_t accent);

/* Render `numSamples` mono q31 samples into `out`. `macros` holds `numMacros` q31 values (0..INT32_MAX = 0..1),
 * `phaseIncrement` is the note's fundamental as the firmware's q32 cycles-per-sample (2^32 = the sample rate),
 * `scratch` is `scratchSize` bytes of host-lent working memory valid for this call only. Returns nonzero while the
 * voice is still sounding; 0 once it has decayed to silence and the host may release it. */
typedef int32_t (*DelugeSourceRenderFn)(const DelugePluginHostApi* api, void* state, const int32_t* macros,
                                        uint32_t phaseIncrement, int32_t* out, uint32_t numSamples, void* scratch);

typedef struct {
	uint32_t abiVersion; /* DELUGE_PLUGIN_ABI_VERSION the plugin was built against */
	const char* name;    /* short display name of the plugin itself */
	uint32_t numModels;  /* entries in `modelInfo` */
	const DelugeSourceModelInfo* modelInfo;
	uint32_t numMacros;      /* <= DELUGE_SOURCE_PLUGIN_MAX_MACROS; what `macros` holds at render time */
	uint32_t voiceStateSize; /* bytes of state the host allocates per voice (4-byte aligned) */
	uint32_t scratchSize;    /* bytes of scratch the host lends to every render call (4-byte aligned) */
	uint32_t maxBlockSize;   /* the most samples one render call accepts */
	DelugeSourceInitFn init;
	DelugeSourceTriggerFn trigger;
	DelugeSourceRenderFn render;
} DelugeSourcePlugin;

#ifdef __cplusplus
}
#endif
