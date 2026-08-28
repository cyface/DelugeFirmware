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
#include "plugin/fx/tape_saturation.h"
#include "plugin/plugin_abi.h"

/// Insert-FX plugins compiled into the firmware. Each is a plain-C kernel under plugin/fx/ described here for the
/// host; everything is constexpr because param.cpp derives XML attribute names from it at compile time. A plugin
/// loaded from the card at runtime would arrive with an equivalent descriptor built from its file header.
namespace deluge::plugin::builtin {

inline constexpr DelugeFxParamInfo kTapeSaturationParams[TAPE_SATURATION_NUM_PARAMS] = {
    [TAPE_SATURATION_PARAM_AMOUNT] = {.name = "Tape",
                                      .shortName = "TAPE",
                                      .fileName = "tapeSaturation",
                                      .defaultValue = static_cast<int32_t>(0x80000000)},
};

inline constexpr DelugeFxPlugin kTapeSaturation = {
    .abiVersion = DELUGE_PLUGIN_ABI_VERSION,
    .name = "Tape",
    .numParams = TAPE_SATURATION_NUM_PARAMS,
    .paramInfo = kTapeSaturationParams,
    .stateSize = sizeof(TapeSaturationState),
    .reset = tape_saturation_reset,
    .isActive = tape_saturation_is_active,
    .render = tape_saturation_render,
};

} // namespace deluge::plugin::builtin
