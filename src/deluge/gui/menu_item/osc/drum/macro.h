/*
 * Copyright (c) 2014-2023 Synthstrom Audible Limited
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
#include "definitions_cxx.hpp"
#include "gui/menu_item/formatted_title.h"
#include "gui/menu_item/source/patched_param.h"
#include "gui/menu_item/value_scaling.h"
#include "gui/ui/sound_editor.h"
#include "modulation/params/param_set.h"
#include "plugin/host/plugin_host.h"
#include "processing/sound/sound.h"

namespace deluge::gui::menu_item::osc::drum {

/// The macro controls of an OscType::DRUM source - the drum source plugin's macros. They sit on the source's
/// otherwise-unused patched params (pulse width, wave index, carrier feedback) so they are automatable and
/// patchable for free:
///  - macro 0 (Tone)  -> LOCAL_OSC_x_PHASE_WIDTH (a "half precision" 0..INT32_MAX param)
///  - macro 1 (Decay) -> LOCAL_OSC_x_WAVE_INDEX
///  - macro 2 (Snap)  -> LOCAL_CARRIER_x_FEEDBACK
/// The names come from the plugin's per-model macro table (a kick's third macro is Drive, a snare's Snappy).
class MacroBase : public menu_item::source::PatchedParam, public FormattedTitle {
public:
	MacroBase(l10n::String name, l10n::String title_format_str, int32_t newP, uint8_t source_id, uint32_t macro)
	    : PatchedParam(name, newP, source_id), FormattedTitle(title_format_str, source_id + 1), macro_(macro) {}

	[[nodiscard]] std::string_view getTitle() const override { return FormattedTitle::title(); }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return SLIDER; }

	[[nodiscard]] std::string_view getName() const override {
		const uint32_t model = soundEditor.currentSound->sources[source_id_].drumModel;
		return deluge::plugin::displayName(deluge::plugin::drumModelInfo(model).macros[macro_]);
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		Sound* sound = static_cast<Sound*>(modControllable);
		return sound->getSynthMode() != SynthMode::FM && sound->sources[source_id_].oscType == OscType::DRUM;
	}

private:
	uint32_t macro_;
};

class Tone final : public MacroBase {
public:
	Tone(l10n::String name, l10n::String title_format_str, int32_t newP, uint8_t source_id)
	    : MacroBase(name, title_format_str, newP, source_id, 0) {}

	int32_t getFinalValue() override { return computeFinalValueForHalfPrecisionMenuItem(this->getValue()); }

	void readCurrentValue() override {
		this->setValue(computeCurrentValueForHalfPrecisionMenuItem(
		    soundEditor.currentParamManager->getPatchedParamSet()->getValue(getP())));
	}
};

class Decay final : public MacroBase {
public:
	Decay(l10n::String name, l10n::String title_format_str, int32_t newP, uint8_t source_id)
	    : MacroBase(name, title_format_str, newP, source_id, 1) {}
};

class Snap final : public MacroBase {
public:
	Snap(l10n::String name, l10n::String title_format_str, int32_t newP, uint8_t source_id)
	    : MacroBase(name, title_format_str, newP, source_id, 2) {}
};

} // namespace deluge::gui::menu_item::osc::drum
