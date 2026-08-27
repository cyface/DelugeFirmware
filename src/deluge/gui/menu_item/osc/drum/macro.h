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
#include "processing/sound/sound.h"

namespace deluge::gui::menu_item::osc::drum {

/// The three macro controls of an OscType::DRUM source. They sit on the source's otherwise-unused patched
/// params (pulse width, wave index, carrier feedback) so they are automatable and patchable for free:
///  - Tone  (Plaits TIMBRE)    -> LOCAL_OSC_x_PHASE_WIDTH (a "half precision" 0..INT32_MAX param)
///  - Decay (Plaits MORPH)     -> LOCAL_OSC_x_WAVE_INDEX
///  - Snap  (Plaits HARMONICS) -> LOCAL_CARRIER_x_FEEDBACK; shown as Drive / Snappy / Noise per model
class MacroBase : public menu_item::source::PatchedParam, public FormattedTitle {
public:
	MacroBase(l10n::String name, l10n::String title_format_str, int32_t newP, uint8_t source_id)
	    : PatchedParam(name, newP, source_id), FormattedTitle(title_format_str, source_id + 1) {}

	[[nodiscard]] std::string_view getTitle() const override { return FormattedTitle::title(); }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return SLIDER; }

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		Sound* sound = static_cast<Sound*>(modControllable);
		return sound->getSynthMode() != SynthMode::FM && sound->sources[source_id_].oscType == OscType::DRUM;
	}
};

class Tone final : public MacroBase {
public:
	using MacroBase::MacroBase;

	int32_t getFinalValue() override { return computeFinalValueForHalfPrecisionMenuItem(this->getValue()); }

	void readCurrentValue() override {
		this->setValue(computeCurrentValueForHalfPrecisionMenuItem(
		    soundEditor.currentParamManager->getPatchedParamSet()->getValue(getP())));
	}
};

class Decay final : public MacroBase {
public:
	using MacroBase::MacroBase;
};

class Snap final : public MacroBase {
public:
	using MacroBase::MacroBase;

	[[nodiscard]] std::string_view getName() const override {
		using enum l10n::String;
		switch (soundEditor.currentSound->sources[source_id_].drumModel) {
		case DrumModel::ANALOG_KICK:
		case DrumModel::SYNTH_KICK:
			return l10n::getView(STRING_FOR_DRIVE);
		case DrumModel::HI_HAT:
			return l10n::getView(STRING_FOR_DRUM_NOISE);
		default:
			return l10n::getView(STRING_FOR_DRUM_SNAPPY);
		}
	}
};

} // namespace deluge::gui::menu_item::osc::drum
