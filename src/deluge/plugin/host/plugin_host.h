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
#include "dsp/stereo_sample.h"
#include "plugin/host/fx_plugin_bank.h"
#include "plugin/plugin_abi.h"
#include <array>
#include <cstdint>
#include <span>

class UnpatchedParamSet;

namespace deluge::plugin {

/// The firmware's side of the plugin ABI: the service table every plugin is handed.
const DelugePluginHostApi& hostApi();

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
