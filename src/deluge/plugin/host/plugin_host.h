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

#pragma once
#include "definitions.h"
#include "dsp/stereo_sample.h"
#include "plugin/host/builtin_sources.h"
#include "plugin/host/fx_plugin_bank.h"
#include "plugin/plugin_abi.h"
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

class UnpatchedParamSet;

namespace deluge::plugin {

/// The firmware's side of the plugin ABI: the service table every plugin is handed.
const DelugePluginHostApi& hostApi();

/// The OLED or 7-segment form of a plugin-supplied name, whichever this Deluge shows.
std::string_view displayName(const DelugePluginName& name);

// ---- source plugins ------------------------------------------------------------------------------------------

/// The source plugin behind OscType::DRUM.
inline constexpr const DelugeSourcePlugin& kDrumSourcePlugin = builtin::kPlaitsDrums;

/// Most scratch a source plugin may ask the host to lend per render call, and the block size the host renders in.
inline constexpr uint32_t kSourcePluginMaxScratchBytes = 2048;
inline constexpr uint32_t kSourcePluginBlockSize = SSI_TX_BUFFER_NUM_SAMPLES;

constexpr bool sourcePluginFitsTheHost(const DelugeSourcePlugin& plugin) {
	return plugin.abiVersion == DELUGE_PLUGIN_ABI_VERSION && plugin.numModels > 0
	       && plugin.numMacros <= DELUGE_SOURCE_PLUGIN_MAX_MACROS && plugin.scratchSize <= kSourcePluginMaxScratchBytes
	       && plugin.maxBlockSize >= kSourcePluginBlockSize && plugin.init != nullptr && plugin.trigger != nullptr
	       && plugin.render != nullptr;
}
static_assert(sourcePluginFitsTheHost(kDrumSourcePlugin));

/// Lookups the rest of the firmware derives model names and XML strings from.
inline const DelugeSourceModelInfo& drumModelInfo(uint32_t model) {
	return kDrumSourcePlugin.modelInfo[model < kDrumSourcePlugin.numModels ? model : 0];
}
/// The model whose XML value is `fileName`, or 0 (the first model) if none matches.
uint32_t drumModelForFileName(std::string_view fileName);

/// One voice of a source plugin: the plugin's per-voice state block (allocated behind this header from the audio
/// heap, one per unison part like a DxVoice) plus which model it was initialised for.
class alignas(8) SourcePluginVoice {
public:
	static SourcePluginVoice* solicit(const DelugeSourcePlugin& plugin);
	static void release(SourcePluginVoice* voice);

	/// Reset the state for `model` with a fresh noise seed and arm a hit at `velocity` (0..127).
	void init(uint32_t model, uint8_t velocity);
	/// Arm another hit on a voice that is still ringing, without resetting it.
	void retrigger(uint8_t velocity);
	/// Render one mono q31 block. `macros` holds plugin().numMacros q31 values (0..INT32_MAX), `phaseIncrement`
	/// the note's q32 cycles per sample. Returns false once the voice has finished and should be released.
	bool render(const int32_t* macros, uint32_t phaseIncrement, int32_t* out, uint32_t numSamples);

	uint32_t model() const { return model_; }
	const DelugeSourcePlugin& plugin() const { return *plugin_; }

private:
	explicit SourcePluginVoice(const DelugeSourcePlugin& plugin) : plugin_(&plugin) {}
	void* state() { return this + 1; }

	const DelugeSourcePlugin* plugin_;
	uint32_t model_ = 0;
};
static_assert(sizeof(SourcePluginVoice) % 8 == 0, "the state block behind the header must stay 8-byte aligned");

// ---- insert FX -----------------------------------------------------------------------------------------------

/// One insert-FX plugin instance: owns the plugin's state block and handles the enable/bypass transition so the
/// kernel only ever sees "render while on".
class FxPluginSlot {
public:
	explicit FxPluginSlot(const DelugeFxPlugin& plugin) : plugin_(&plugin) {}

	/// Render in place when `enabled`; otherwise bypass and forget the state so the next enable starts clean
	/// (a stale filter history from seconds ago would otherwise thump on re-entry).
	void process(std::span<StereoSample> buffer, const int32_t* params, bool enabled, uint32_t levelShift);

	const DelugeFxPlugin& plugin() const { return *plugin_; }

private:
	const DelugeFxPlugin* plugin_; // pointer, not reference, so owners stay assignable
	bool active_ = false;
	alignas(8) uint8_t state_[kFxPluginMaxStateBytes];
};

/// Every registered insert-FX plugin, instanced once per ModControllableAudio, fed from the param bank.
class FxPluginChain {
public:
	FxPluginChain();

	/// Run each plugin in registry order over `buffer`, reading its params from the bank slots it owns in
	/// `unpatched`. `levelShift` describes this insertion point (see DelugeFxContext).
	void process(std::span<StereoSample> buffer, UnpatchedParamSet& unpatched, uint32_t levelShift);

private:
	std::array<FxPluginSlot, kNumBuiltinFxPlugins> slots_;
};

} // namespace deluge::plugin
