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
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "processing/sound/sound.h"
#include "processing/source.h"

namespace deluge::gui::menu_item::osc::drum {

/// Which Plaits drum model an OscType::DRUM source plays.
class Model final : public Selection, public FormattedTitle {
public:
	Model(l10n::String name, l10n::String title_format_str, uint8_t source_id)
	    : Selection(name), FormattedTitle(title_format_str, source_id + 1), sourceId_{source_id} {}

	[[nodiscard]] std::string_view getTitle() const override { return FormattedTitle::title(); }

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {
		    l10n::getView(STRING_FOR_DRUM_MODEL_808_KICK),  //<
		    l10n::getView(STRING_FOR_DRUM_MODEL_808_SNARE), //<
		    l10n::getView(STRING_FOR_DRUM_MODEL_HI_HAT),    //<
		    l10n::getView(STRING_FOR_DRUM_MODEL_909_KICK),  //<
		    l10n::getView(STRING_FOR_DRUM_MODEL_909_SNARE), //<
		    l10n::getView(STRING_FOR_DRUM_MODEL_HI_HAT_2),  //<
		};
	}

	void readCurrentValue() override {
		setValue(util::to_underlying(soundEditor.currentSound->sources[sourceId_].drumModel));
	}

	void writeCurrentValue() override {
		Source& source = soundEditor.currentSound->sources[sourceId_];
		auto newModel = getValue<DrumModel>();
		if (newModel != source.drumModel) {
			// Voices re-initialise their model on the next note-on; stop the old ones cleanly.
			soundEditor.currentSound->killAllVoices();
			source.drumModel = newModel;
		}
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		Sound* sound = static_cast<Sound*>(modControllable);
		return sound->getSynthMode() != SynthMode::FM && sound->sources[sourceId_].oscType == OscType::DRUM;
	}

	bool wrapAround() override {
		return parent != nullptr && parent->renderingStyle() == Submenu::RenderingStyle::HORIZONTAL;
	}

private:
	uint8_t sourceId_;
};

} // namespace deluge::gui::menu_item::osc::drum
