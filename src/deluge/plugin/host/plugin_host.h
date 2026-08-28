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
#include "plugin/plugin_abi.h"
#include <cstdint>
#include <span>

namespace deluge::plugin {

/// The firmware's side of the plugin ABI: the service table every plugin is handed.
const DelugePluginHostApi& hostApi();

/// Largest per-instance state block a slot can hold. Built-in plugins static_assert against it
/// (builtin_fx.cpp); a loaded plugin whose descriptor asks for more is refused.
constexpr uint32_t kFxPluginMaxStateBytes = 64;

/// One insert-FX plugin instance living inside a ModControllableAudio: owns the plugin's state block and
/// handles the enable/bypass transition so the kernel only ever sees "render while on".
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

} // namespace deluge::plugin
