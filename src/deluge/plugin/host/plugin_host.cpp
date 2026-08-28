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
#include "modulation/params/param_set.h"
#include "util/functions.h"
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

const DelugePluginHostApi kHostApi = {
    .version = DELUGE_PLUGIN_ABI_VERSION,
    .tanhAntialiased = hostTanhAntialiased,
    .tanhUnknown = hostTanhUnknown,
};

// A plugin renders straight onto the firmware's buffer, so the two frame layouts must agree.
static_assert(sizeof(DelugePluginStereoSample) == sizeof(StereoSample));
static_assert(offsetof(DelugePluginStereoSample, l) == offsetof(StereoSample, l));
static_assert(offsetof(DelugePluginStereoSample, r) == offsetof(StereoSample, r));

} // namespace

const DelugePluginHostApi& hostApi() {
	return kHostApi;
}

namespace {
template <size_t... I>
std::array<FxPluginSlot, sizeof...(I)> makeSlots(std::index_sequence<I...>) {
	return {FxPluginSlot(*kBuiltinFxPlugins[I])...};
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
