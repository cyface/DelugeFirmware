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
#include "plugin/plugin_abi.h"
#include "plugin/source/plaits_drums.h"

/// Source plugins compiled into the firmware, described for the host the way builtin_fx.h describes the insert
/// FX. Everything here is plain data (names, sizes, three entry points) so a plugin loaded from the card could hand
/// the host the same descriptor built from its file header.
namespace deluge::plugin::builtin {

inline constexpr DelugePluginName kDrumMacroTone = {"Tone", "TONE"};
inline constexpr DelugePluginName kDrumMacroDecay = {"Decay", "DCAY"};
inline constexpr DelugePluginName kDrumMacroDrive = {"Drive", "DRIV"};
inline constexpr DelugePluginName kDrumMacroSnappy = {"Snappy", "SNAP"};
inline constexpr DelugePluginName kDrumMacroNoise = {"Noise", "NOIS"};

inline constexpr DelugeSourceModelInfo kPlaitsDrumModels[PLAITS_DRUMS_NUM_MODELS] = {
    [PLAITS_DRUMS_MODEL_808_KICK] = {.name = {"808 Kick", "8KIK"},
                                     .fileName = "808kick",
                                     .macros = {kDrumMacroTone, kDrumMacroDecay, kDrumMacroDrive}},
    [PLAITS_DRUMS_MODEL_808_SNARE] = {.name = {"808 Snare", "8SNR"},
                                      .fileName = "808snare",
                                      .macros = {kDrumMacroTone, kDrumMacroDecay, kDrumMacroSnappy}},
    [PLAITS_DRUMS_MODEL_HI_HAT] = {.name = {"Hi-hat", "HHAT"},
                                   .fileName = "hihat",
                                   .macros = {kDrumMacroTone, kDrumMacroDecay, kDrumMacroNoise}},
    [PLAITS_DRUMS_MODEL_909_KICK] = {.name = {"909 Kick", "9KIK"},
                                     .fileName = "909kick",
                                     .macros = {kDrumMacroTone, kDrumMacroDecay, kDrumMacroDrive}},
    [PLAITS_DRUMS_MODEL_909_SNARE] = {.name = {"909 Snare", "9SNR"},
                                      .fileName = "909snare",
                                      .macros = {kDrumMacroTone, kDrumMacroDecay, kDrumMacroSnappy}},
    [PLAITS_DRUMS_MODEL_HI_HAT_2] = {.name = {"Hi-hat 2", "HAT2"},
                                     .fileName = "hihat2",
                                     .macros = {kDrumMacroTone, kDrumMacroDecay, kDrumMacroSnappy}},
};

inline constexpr DelugeSourcePlugin kPlaitsDrums = {
    .abiVersion = DELUGE_PLUGIN_ABI_VERSION,
    .name = "Drum",
    .numModels = PLAITS_DRUMS_NUM_MODELS,
    .modelInfo = kPlaitsDrumModels,
    .numMacros = PLAITS_DRUMS_NUM_MACROS,
    .voiceStateSize = sizeof(PlaitsDrumsVoice),
    .scratchSize = PLAITS_DRUMS_SCRATCH_BYTES,
    .maxBlockSize = PLAITS_DRUMS_MAX_BLOCK_SIZE,
    .init = plaits_drums_init,
    .trigger = plaits_drums_trigger,
    .render = plaits_drums_render,
};

} // namespace deluge::plugin::builtin
