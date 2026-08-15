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
#include "gui/menu_item/menu_item.h"
#include "gui/ui_timer_manager.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/display/seven_segment.h"
#include "model/settings/runtime_feature_settings.h"
#include "power.h"
#include <cstddef>
#include <cstdio>

namespace deluge::gui::menu_item::battery {
class Level final : public MenuItem {
public:
	using MenuItem::MenuItem;

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) override {
		return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::ShowBatteryLevel);
	}
	void drawPixelsForOled() override {
		deluge::hid::display::oled_canvas::Canvas& canvas = hid::display::OLED::main;
		char buffer[kStringSize];
		getBatteryString(buffer);
		canvas.drawStringCentredShrinkIfNecessary(buffer, 22, 18, 20);
	}

	void beginSession(MenuItem* navigatedBackwardFrom) override {
		drawValue();
		// Start the timer for updates
		uiTimerManager.setTimer(TimerName::UI_SPECIFIC, 500);
	}

	void drawValue() {
		char buffer[kStringSize];
		getBatteryString(buffer);
		display->setScrollingText(buffer);
	}

	ActionResult timerCallback() override {
		drawValue();
		uiTimerManager.setTimer(TimerName::UI_SPECIFIC, 500);
		return ActionResult::DEALT_WITH;
	}

private:
	/// Enough for the longest form, "CHG 100% 999m (5008mV)".
	static constexpr std::size_t kStringSize = 32;

	/**
	 * Formats battery information into a string buffer.
	 *
	 * Charge state is tracked continuously by deluge::power rather than worked out here, so the reading is
	 * already correct the moment this menu opens rather than needing seconds of observation first.
	 *
	 * Raw mV is always shown: on battery it is the cell, on external power it is the supply rail, and in both
	 * cases it is the only number here that is a direct measurement.
	 *
	 * @param buffer Output buffer for the formatted string, of at least kStringSize characters.
	 */
	void getBatteryString(char* buffer) {
		const int32_t percent = power::percent();

		switch (power::state()) {
		case power::State::OnBattery:
			snprintf(buffer, kStringSize, "%d%% (%dmV)", (int)percent, batteryMV);
			break;

		case power::State::Charging:
			// The charger's power path hides the cell, so there is no live percentage to show, and no way to
			// tell a full cell from an empty one until it is unplugged. Report the last reading taken on
			// battery along with its age, rather than the rail voltage dressed up as a charge level — that is
			// what used to make this menu read "100% FULL" from the moment you plugged in.
			if (percent == power::kPercentUnknown) {
				snprintf(buffer, kStringSize, "CHG (%dmV)", batteryMV);
			}
			else {
				snprintf(buffer, kStringSize, "CHG %d%% %dm (%dmV)", (int)percent, (int)power::minutesOnExternalPower(),
				         batteryMV);
			}
			break;
		}
	}
};
} // namespace deluge::gui::menu_item::battery
