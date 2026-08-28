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

#include "plugin/host/plugin_host.h"
#include "hid/display/display.h"
#include "memory/memory_allocator_interface.h"
#include "modulation/params/param_set.h"
#include "util/functions.h"
#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace deluge::plugin {

namespace {

// The firmware's always_inline table helpers, given a real address so a plugin can reach them through the API
// table. One call per sample per channel; the #37 spike measured this pattern as free once warm.
[[gnu::noinline]] int32_t hostTanhAntialiased(int32_t input, uint32_t* lastWorkingValue, uint32_t saturationAmount) {
	return getTanHAntialiased(input, lastWorkingValue, saturationAmount);
}

[[gnu::noinline]] int32_t hostTanhUnknown(int32_t input, uint32_t saturationAmount) {
	return getTanHUnknown(input, saturationAmount);
}

[[gnu::noinline]] float hostExp2f(float x) {
	return exp2f(x);
}

// Float sine table for source kernels (Plaits' lut_sine layout), filled on first use.
float sineLut[DELUGE_PLUGIN_SINE_LUT_SIZE + 1];
bool sineLutReady = false;

void ensureSineLut() {
	if (sineLutReady) {
		return;
	}
	for (size_t i = 0; i <= DELUGE_PLUGIN_SINE_LUT_SIZE; i++) {
		sineLut[i] = sinf(2.0f * 3.14159265358979323846f * static_cast<float>(i)
		                  / static_cast<float>(DELUGE_PLUGIN_SINE_LUT_SIZE));
	}
	sineLutReady = true;
}

const DelugePluginHostApi kHostApi = {
    .version = DELUGE_PLUGIN_ABI_VERSION,
    .tanhAntialiased = hostTanhAntialiased,
    .tanhUnknown = hostTanhUnknown,
    .exp2f = hostExp2f,
    .sineLut = sineLut,
};

// A plugin renders straight onto the firmware's buffer, so the two frame layouts must agree.
static_assert(sizeof(DelugePluginStereoSample) == sizeof(StereoSample));
static_assert(offsetof(DelugePluginStereoSample, l) == offsetof(StereoSample, l));
static_assert(offsetof(DelugePluginStereoSample, r) == offsetof(StereoSample, r));

} // namespace

const DelugePluginHostApi& hostApi() {
	return kHostApi;
}

std::string_view displayName(const DelugePluginName& name) {
	return display->haveOLED() ? name.name : name.shortName;
}

// ---- source plugins ------------------------------------------------------------------------------------------

namespace {
// Which descriptor each consumer sees. They start as the built-ins and are only ever moved by the boot-time scan
// of PLUGINS/ (plugin_loader.cpp), before any voice or FX chain has read them.
const DelugeSourcePlugin* activeDrumPlugin = &kBuiltinDrumSourcePlugin;
std::array<const DelugeFxPlugin*, kNumBuiltinFxPlugins> activeFxPlugins = kBuiltinFxPlugins;
} // namespace

const DelugeSourcePlugin& drumSourcePlugin() {
	return *activeDrumPlugin;
}

const DelugeFxPlugin& activeFxPlugin(uint32_t index) {
	return *activeFxPlugins[index];
}

void installLoadedSourcePlugin(const DelugeSourcePlugin& plugin) {
	activeDrumPlugin = &plugin;
}

void installLoadedFxPlugin(uint32_t index, const DelugeFxPlugin& plugin) {
	activeFxPlugins[index] = &plugin;
}

uint32_t drumModelForFileName(std::string_view fileName) {
	const DelugeSourcePlugin& plugin = drumSourcePlugin();
	for (uint32_t i = 0; i < plugin.numModels; i++) {
		if (fileName == plugin.modelInfo[i].fileName) {
			return i;
		}
	}
	return 0;
}

namespace {
// Working memory lent to every source-plugin render call. Rendering happens on the audio thread only.
alignas(CACHE_LINE_SIZE) uint8_t sourcePluginScratch[kSourcePluginMaxScratchBytes];

int32_t accentFromVelocity(uint8_t velocity) {
	return static_cast<int32_t>(std::min<uint32_t>(velocity, 127) * (0x7FFFFFFFu / 127u));
}
} // namespace

SourcePluginVoice* SourcePluginVoice::solicit(const DelugeSourcePlugin& plugin) {
	ensureSineLut();
	void* memory = allocMaxSpeed(sizeof(SourcePluginVoice) + plugin.voiceStateSize);
	if (memory == nullptr) {
		return nullptr;
	}
	return new (memory) SourcePluginVoice(plugin);
}

void SourcePluginVoice::release(SourcePluginVoice* voice) {
	delugeDealloc(voice);
}

void SourcePluginVoice::init(uint32_t model, uint8_t velocity) {
	model_ = model;
	plugin_->init(state(), model, static_cast<uint32_t>(getNoise()));
	plugin_->trigger(state(), accentFromVelocity(velocity));
}

void SourcePluginVoice::retrigger(uint8_t velocity) {
	plugin_->trigger(state(), accentFromVelocity(velocity));
}

bool SourcePluginVoice::render(const int32_t* macros, uint32_t phaseIncrement, int32_t* out, uint32_t numSamples) {
	return plugin_->render(&kHostApi, state(), macros, phaseIncrement, out, numSamples, sourcePluginScratch) != 0;
}

// ---- insert FX -----------------------------------------------------------------------------------------------

namespace {
template <size_t... I>
std::array<FxPluginSlot, sizeof...(I)> makeSlots(std::index_sequence<I...>) {
	return {FxPluginSlot(activeFxPlugin(I))...};
}
} // namespace

FxPluginChain::FxPluginChain() : slots_(makeSlots(std::make_index_sequence<kNumBuiltinFxPlugins>{})) {
}

void FxPluginChain::process(std::span<StereoSample> buffer, UnpatchedParamSet& unpatched, uint32_t levelShift) {
	uint32_t bankIndex = 0;
	for (uint32_t i = 0; i < kNumBuiltinFxPlugins; i++) {
		const DelugeFxPlugin& plugin = slots_[i].plugin();
		int32_t params[kMaxParamsPerFxPlugin];
		for (uint32_t k = 0; k < plugin.numParams; k++) {
			params[k] = unpatched.getValue(fxBankParamId(bankIndex + k));
		}
		bankIndex += plugin.numParams;
		bool enabled = (plugin.isActive == nullptr) || (plugin.isActive(params) != 0);
		slots_[i].process(buffer, params, enabled, levelShift);
	}
}

// ---- self-check ----------------------------------------------------------------------------------------------

namespace {
// One block is enough to tell two kernels apart: every model here runs its excitation, its filters and its
// overdrive within the first few dozen samples.
constexpr uint32_t kSelfCheckFrames = 64;
constexpr uint32_t kSelfCheckStateBytes = 1024;

uint32_t fold(uint32_t checksum, int32_t value) {
	return (checksum * 31u) ^ static_cast<uint32_t>(value);
}

// A deterministic test signal: the same numbers on every boot and every device, so the checksums compare.
int32_t nextTestSample(uint32_t& seed) {
	seed = seed * 1664525u + 1013904223u;
	return static_cast<int32_t>(seed) >> 2;
}
} // namespace

uint32_t selfCheck(const DelugeFxPlugin& plugin) {
	if (plugin.reset == nullptr || plugin.render == nullptr || plugin.stateSize > kSelfCheckStateBytes
	    || plugin.numParams > kMaxParamsPerFxPlugin) {
		return 0;
	}
	alignas(8) uint8_t state[kSelfCheckStateBytes];
	DelugePluginStereoSample buffer[kSelfCheckFrames];
	uint32_t seed = 0x01234567u;
	for (uint32_t i = 0; i < kSelfCheckFrames; i++) {
		buffer[i].l = nextTestSample(seed);
		buffer[i].r = nextTestSample(seed);
	}
	int32_t params[kMaxParamsPerFxPlugin] = {};
	for (uint32_t i = 0; i < plugin.numParams; i++) {
		params[i] = 0x20000000; // well above any "off at the minimum", so the kernel actually does its work
	}
	plugin.reset(state);
	DelugeFxContext context = {.levelShift = 8};
	plugin.render(&kHostApi, state, params, &context, buffer, kSelfCheckFrames);
	uint32_t checksum = 0;
	for (uint32_t i = 0; i < kSelfCheckFrames; i++) {
		checksum = fold(fold(checksum, buffer[i].l), buffer[i].r);
	}
	return checksum != 0 ? checksum : 1u; // 0 is reserved for "did not run"
}

uint32_t selfCheck(const DelugeSourcePlugin& plugin) {
	if (!sourcePluginFitsTheHost(plugin) || plugin.voiceStateSize > kSelfCheckStateBytes) {
		return 0;
	}
	ensureSineLut();
	alignas(8) uint8_t state[kSelfCheckStateBytes];
	int32_t out[kSelfCheckFrames];
	int32_t macros[DELUGE_SOURCE_PLUGIN_MAX_MACROS] = {0x40000000, 0x40000000, 0x40000000};
	plugin.init(state, 0, 0x9E3779B9u); // model 0, a fixed noise seed: the run has to repeat exactly
	plugin.trigger(state, 0x60000000);
	plugin.render(&kHostApi, state, macros, 0x00100000u, out, kSelfCheckFrames, sourcePluginScratch);
	uint32_t checksum = 0;
	for (uint32_t i = 0; i < kSelfCheckFrames; i++) {
		checksum = fold(checksum, out[i]);
	}
	return checksum != 0 ? checksum : 1u;
}

void FxPluginSlot::process(std::span<StereoSample> buffer, const int32_t* params, bool enabled, uint32_t levelShift) {
	if (!enabled) {
		active_ = false;
		return;
	}
	if (!active_) {
		active_ = true;
		plugin_->reset(state_);
	}
	DelugeFxContext context = {.levelShift = levelShift};
	plugin_->render(&kHostApi, state_, params, &context, reinterpret_cast<DelugePluginStereoSample*>(buffer.data()),
	                static_cast<uint32_t>(buffer.size()));
}

} // namespace deluge::plugin
