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

/* The Mutable Instruments Plaits drum models (808 / 909 kicks and snares, two hi-hats) as a tier-1 source-plugin
 * kernel. Plain C port of Emilie Gillet's MIT-licensed C++ (see README.md in this directory for provenance and
 * the changes made); the models themselves are untouched, only the language and the plugin contract differ. */
#pragma once
#include "plugin/plugin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Model indices, in menu order. Also the order of the descriptor's modelInfo table. */
enum {
	PLAITS_DRUMS_MODEL_808_KICK = 0,
	PLAITS_DRUMS_MODEL_808_SNARE = 1,
	PLAITS_DRUMS_MODEL_HI_HAT = 2,
	PLAITS_DRUMS_MODEL_909_KICK = 3,
	PLAITS_DRUMS_MODEL_909_SNARE = 4,
	PLAITS_DRUMS_MODEL_HI_HAT_2 = 5,
	PLAITS_DRUMS_NUM_MODELS = 6,
};

/* The three Plaits macros, q31 0..1 at render time: TONE is Plaits' TIMBRE, DECAY its MORPH, SNAP the
 * model-specific HARMONICS control (attack FM + drive on the kicks, snappiness on the snares, extra noise on the
 * hi-hats). */
enum {
	PLAITS_DRUMS_MACRO_TONE = 0,
	PLAITS_DRUMS_MACRO_DECAY = 1,
	PLAITS_DRUMS_MACRO_SNAP = 2,
	PLAITS_DRUMS_NUM_MACROS = 3,
};

#define PLAITS_DRUMS_MAX_BLOCK_SIZE 128u
/* A float render buffer plus the two temporaries the ring-modulated hi-hat needs. */
#define PLAITS_DRUMS_SCRATCH_BYTES (3u * PLAITS_DRUMS_MAX_BLOCK_SIZE * 4u)

/* --- Building blocks (stmlib) ------------------------------------------------------------------------------- */

typedef struct {
	float g, gi, state;
} PdOnePole;

typedef struct {
	float g, r, h, state1, state2;
} PdSvf;

/* PolyBLEP saw / square (plaits Oscillator). */
typedef struct {
	float phase, nextSample;
	int32_t high;
	float frequency, pw;
} PdOscillator;

typedef struct {
	float preGain, postGain;
} PdOverdrive;

/* --- Models --------------------------------------------------------------------------------------------------- */

typedef struct {
	int32_t pulseRemainingSamples, fmPulseRemainingSamples;
	float pulse, pulseHeight, pulseLp, fmPulseLp, retrigPulse, lpOut, toneLp;
	PdSvf resonator;
	PdOverdrive overdrive;
} PdAnalogBassDrum;

#define PD_ANALOG_SNARE_NUM_MODES 5
typedef struct {
	int32_t pulseRemainingSamples;
	float pulse, pulseHeight, pulseLp, noiseEnvelope;
	PdSvf resonator[PD_ANALOG_SNARE_NUM_MODES];
	PdSvf noiseFilter;
} PdAnalogSnareDrum;

typedef struct {
	float envelope, noiseClock, noiseSample;
	union {
		uint32_t squarePhase[6]; /* six schmitt-trigger square waves (808) */
		PdOscillator ringMod[6]; /* three square x saw pairs (Plaits' "hi-hat 2") */
	} metallic;
	PdSvf noiseColorationSvf;
	PdSvf hpf;
} PdHiHat;

typedef struct {
	float f0, phase, phaseNoise;
	float fm, fmLp, bodyEnv, bodyEnvLp, transientEnv, transientEnvLp, toneLp;
	float clickLp, clickHp;
	PdSvf clickFilter;
	float noiseLp, noiseHp;
	int32_t bodyEnvPulseWidth, fmPulseWidth;
} PdSyntheticBassDrum;

typedef struct {
	float phase[2];
	float drumAmplitude, snareAmplitude, fm;
	int32_t holdCounter;
	PdOnePole drumLp, snareHp;
	PdSvf snareLp;
} PdSyntheticSnareDrum;

/* One voice: which model, the trigger / accent / silence bookkeeping Plaits' voice wrapper would do, the voice's
 * own noise generator, and the model state. */
typedef struct {
	uint32_t model;
	int32_t triggerPending;
	uint32_t silentBlocks;
	float accent;
	uint32_t rng;
	union {
		PdAnalogBassDrum analogKick;
		PdAnalogSnareDrum analogSnare;
		PdHiHat hiHat;
		PdSyntheticBassDrum synthKick;
		PdSyntheticSnareDrum synthSnare;
	} m;
} PlaitsDrumsVoice;

void plaits_drums_init(void* state, uint32_t model, uint32_t seed);
void plaits_drums_trigger(void* state, int32_t accent);
int32_t plaits_drums_render(const DelugePluginHostApi* api, void* state, const int32_t* macros, uint32_t phaseIncrement,
                            int32_t* out, uint32_t numSamples, void* scratch);

#ifdef __cplusplus
}
#endif
