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
#include "modulation/params/param.h"
#include "plugin/host/builtin_fx.h"
#include <array>
#include <cstdint>
#include <string_view>

/// The FX-plugin param bank: the firmware reserves params::kNumFxPluginParams shared unpatched params
/// (UNPATCHED_FX_PLUGIN_PARAM_FIRST onwards) and this registry hands them out, in order, to the built-in plugins.
/// Everything that used to be a hand edit per param - display name, XML attribute, default, menu entry, automation
/// entry - is derived from the plugins' DelugeFxParamInfo through the lookups below.
namespace deluge::plugin {

/// Built-in insert-FX plugins in signal-chain order. Adding one: write the kernel, describe it in builtin_fx.h,
/// append it here, and bump params::kNumFxPluginParams by its param count (the static_assert below checks).
inline constexpr std::array<const DelugeFxPlugin*, 1> kBuiltinFxPlugins = {&builtin::kTapeSaturation};
inline constexpr uint32_t kNumBuiltinFxPlugins = kBuiltinFxPlugins.size();

/// Largest per-instance state block a slot can hold, and the most params one plugin may declare.
inline constexpr uint32_t kFxPluginMaxStateBytes = 64;
inline constexpr uint32_t kMaxParamsPerFxPlugin = 8;

/// Bank offset of a plugin's first param (plugins occupy the bank contiguously, in registry order).
constexpr uint32_t fxPluginFirstParam(uint32_t pluginIndex) {
	uint32_t n = 0;
	for (uint32_t i = 0; i < pluginIndex; i++) {
		n += kBuiltinFxPlugins[i]->numParams;
	}
	return n;
}

inline constexpr uint32_t kFxPluginBankSize = fxPluginFirstParam(kNumBuiltinFxPlugins);
static_assert(kFxPluginBankSize == modulation::params::kNumFxPluginParams,
              "params::kNumFxPluginParams must equal the total params of the registered FX plugins");

constexpr bool builtinFxPluginsFitTheHost() {
	for (const DelugeFxPlugin* plugin : kBuiltinFxPlugins) {
		if (plugin->abiVersion != DELUGE_PLUGIN_ABI_VERSION || plugin->numParams > kMaxParamsPerFxPlugin
		    || plugin->stateSize > kFxPluginMaxStateBytes || plugin->reset == nullptr || plugin->render == nullptr) {
			return false;
		}
	}
	return true;
}
static_assert(builtinFxPluginsFitTheHost());

/// Which plugin owns a bank slot, and which of its params it is.
struct FxBankSlot {
	const DelugeFxPlugin* plugin;
	uint32_t paramIndex;
};

constexpr FxBankSlot fxBankSlot(uint32_t bankIndex) {
	uint32_t base = 0;
	for (const DelugeFxPlugin* plugin : kBuiltinFxPlugins) {
		if (bankIndex < base + plugin->numParams) {
			return {plugin, bankIndex - base};
		}
		base += plugin->numParams;
	}
	return {nullptr, 0};
}

constexpr const DelugeFxParamInfo& fxBankParamInfo(uint32_t bankIndex) {
	FxBankSlot slot = fxBankSlot(bankIndex);
	return slot.plugin->paramInfo[slot.paramIndex];
}

/// The bank index whose XML attribute is `fileName`, or -1.
constexpr int32_t fxBankIndexForFileName(std::string_view fileName) {
	for (uint32_t i = 0; i < kFxPluginBankSize; i++) {
		if (fileName == fxBankParamInfo(i).fileName) {
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

/// The unpatched param ID (without UNPATCHED_START) of a bank slot.
constexpr modulation::params::ParamType fxBankParamId(uint32_t bankIndex) {
	return modulation::params::UNPATCHED_FX_PLUGIN_PARAM_FIRST + bankIndex;
}

} // namespace deluge::plugin
