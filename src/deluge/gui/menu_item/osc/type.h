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
#include "gui/menu_item/submenu.h"
#include "gui/ui/sound_editor.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound.h"
#include "processing/source.h"
#include "util/comparison.h"
#include <algorithm>

#include <hid/display/oled.h>

extern gui::menu_item::Submenu dxMenu;

namespace deluge::gui::menu_item::osc {
class Type final : public Selection, public FormattedTitle {
public:
	Type(l10n::String name, l10n::String title_format_str, uint8_t source_id)
	    : Selection(name), FormattedTitle(title_format_str, source_id + 1), sourceId_{source_id} {};
	void beginSession(MenuItem* navigatedBackwardFrom) override { Selection::beginSession(navigatedBackwardFrom); }

	bool mayUseDx() const { return !soundEditor.editingKit() && sourceId_ == 0; }

	/// Drum models are hidden until the community feature is enabled, but a preset that already uses one
	/// must still be able to display (and keep) its type.
	bool mayUseDrum() const {
		return runtimeFeatureSettings.get(RuntimeFeatureSettingType::EnableDrumModels) == RuntimeFeatureStateToggle::On
		       || soundEditor.currentSound->sources[sourceId_].oscType == OscType::DRUM;
	}

	/// The OscTypes offered by this menu, in display order. Indexes into this list are the Selection's values.
	deluge::vector<OscType> visibleTypes() const {
		deluge::vector<OscType> types = {
		    OscType::SINE, OscType::TRIANGLE,     OscType::SQUARE,    OscType::ANALOG_SQUARE,
		    OscType::SAW,  OscType::ANALOG_SAW_2, OscType::WAVETABLE,
		};
		if (soundEditor.currentSound->getSynthMode() == SynthMode::RINGMOD) {
			return types;
		}
		types.push_back(OscType::SAMPLE);
		if (mayUseDx()) {
			types.push_back(OscType::DX7);
		}
		if (mayUseDrum()) {
			types.push_back(OscType::DRUM);
		}
		if (AudioEngine::micPluggedIn || AudioEngine::lineInPluggedIn) {
			types.push_back(OscType::INPUT_L);
			types.push_back(OscType::INPUT_R);
			types.push_back(OscType::INPUT_STEREO);
		}
		else {
			types.push_back(OscType::INPUT_L);
		}
		return types;
	}

	void readCurrentValue() override {
		const OscType current = soundEditor.currentSound->sources[sourceId_].oscType;
		const auto types = visibleTypes();
		int32_t index = 0;
		for (size_t i = 0; i < types.size(); i++) {
			if (types[i] == current) {
				index = static_cast<int32_t>(i);
				break;
			}
		}
		setValue(index);
	}
	void writeCurrentValue() override {
		OscType oldValue = soundEditor.currentSound->sources[sourceId_].oscType;
		const auto types = visibleTypes();
		const int32_t index = std::clamp<int32_t>(getValue(), 0, static_cast<int32_t>(types.size()) - 1);
		OscType newValue = types[index];

		const auto needs_unassignment = {
		    OscType::INPUT_L,
		    OscType::INPUT_R,
		    OscType::INPUT_STEREO,
		    OscType::SAMPLE,
		    OscType::DX7,
		    OscType::DRUM,

		    // Haven't actually really determined if this needs to be here - maybe not?
		    OscType::WAVETABLE,
		};

		if (util::one_of(oldValue, needs_unassignment) || util::one_of(newValue, needs_unassignment)) {
			soundEditor.currentSound->killAllVoices();
		}

		soundEditor.currentSound->sources[sourceId_].setOscType(newValue);

		if (oldValue == OscType::SQUARE || newValue == OscType::SQUARE) {
			soundEditor.currentSound->setupPatchingForAllParamManagers(currentSong);
		}
	}

	[[nodiscard]] std::string_view getTitle() const override { return FormattedTitle::title(); }

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		const bool stereoInputs = AudioEngine::micPluggedIn || AudioEngine::lineInPluggedIn;
		deluge::vector<std::string_view> options;
		for (OscType type : visibleTypes()) {
			switch (type) {
			case OscType::SINE:
				options.emplace_back(l10n::getView(STRING_FOR_SINE));
				break;
			case OscType::TRIANGLE:
				options.emplace_back(l10n::getView(STRING_FOR_TRIANGLE));
				break;
			case OscType::SQUARE:
				options.emplace_back(l10n::getView(STRING_FOR_SQUARE));
				break;
			case OscType::ANALOG_SQUARE:
				options.emplace_back(l10n::getView(STRING_FOR_ANALOG_SQUARE));
				break;
			case OscType::SAW:
				options.emplace_back(l10n::getView(STRING_FOR_SAW));
				break;
			case OscType::ANALOG_SAW_2:
				options.emplace_back(l10n::getView(STRING_FOR_ANALOG_SAW));
				break;
			case OscType::WAVETABLE:
				options.emplace_back(l10n::getView(STRING_FOR_WAVETABLE));
				break;
			case OscType::SAMPLE:
				options.emplace_back(l10n::getView(STRING_FOR_SAMPLE));
				break;
			case OscType::DX7:
				options.emplace_back(l10n::getView(STRING_FOR_DX7));
				break;
			case OscType::DRUM:
				options.emplace_back(l10n::getView(STRING_FOR_DRUM));
				break;
			case OscType::INPUT_L:
				options.emplace_back(l10n::getView(stereoInputs ? STRING_FOR_INPUT_LEFT : STRING_FOR_INPUT));
				break;
			case OscType::INPUT_R:
				options.emplace_back(l10n::getView(STRING_FOR_INPUT_RIGHT));
				break;
			case OscType::INPUT_STEREO:
				options.emplace_back(l10n::getView(STRING_FOR_INPUT_STEREO));
				break;
			}
		}
		return options;
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		Sound* sound = static_cast<Sound*>(modControllable);
		return (sound->getSynthMode() != SynthMode::FM);
	}

	MenuItem* selectButtonPress() override {
		if (soundEditor.currentSound->sources[sourceId_].oscType != OscType::DX7) {
			return nullptr;
		}
		return &dxMenu;
	}

	[[nodiscard]] bool showColumnLabel() const override { return false; }

	void renderInHorizontalMenu(const SlotPosition& slot) override {
		oled_canvas::Canvas& image = OLED::main;

		const OscType osc_type = soundEditor.currentSound->sources[sourceId_].oscType;
		if (osc_type == OscType::DX7 || osc_type == OscType::DRUM) {
			const auto option = getOptions(OptType::FULL)[getValue()].data();
			return image.drawStringCentered(option, slot.start_x, slot.start_y + kHorizontalMenuSlotYOffset + 5,
			                                kTextTitleSpacingX, kTextTitleSizeY, slot.width);
		}

		const Icon& icon = [&] {
			switch (osc_type) {
			case OscType::SINE:
				return OLED::sineIcon;
			case OscType::TRIANGLE:
				return OLED::triangleIcon;
			case OscType::SQUARE:
			case OscType::ANALOG_SQUARE:
				return OLED::squareIcon;
			case OscType::SAW:
			case OscType::ANALOG_SAW_2:
				return OLED::sawIcon;
			case OscType::SAMPLE:
				return OLED::sampleIcon;
			case OscType::INPUT_STEREO:
			case OscType::INPUT_L:
			case OscType::INPUT_R:
				return AudioEngine::lineInPluggedIn ? OLED::inputIcon : OLED::micIcon;
			case OscType::WAVETABLE:
				return OLED::wavetableIcon;
			default:
				return OLED::sineIcon;
			}
		}();

		image.drawIconCentered(icon, slot.start_x, slot.width, slot.start_y + kHorizontalMenuSlotYOffset + 2);

		if (osc_type == OscType::ANALOG_SQUARE || osc_type == OscType::ANALOG_SAW_2) {
			const int32_t x = slot.start_x + 4;
			constexpr int32_t y = OLED_MAIN_HEIGHT_PIXELS - kTextSpacingY - 8;
			image.clearAreaExact(x - 1, y - 1, x + kTextSpacingX + 1, y + kTextSpacingY + 1);
			image.drawChar('A', x, y, kTextSpacingX, kTextSpacingY);
		}
	}

	bool wrapAround() override {
		return parent != nullptr && parent->renderingStyle() == Submenu::RenderingStyle::HORIZONTAL;
	}

private:
	uint8_t sourceId_;
};

} // namespace deluge::gui::menu_item::osc
