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

#include "gui/menu_item/fx_plugin_param.h"
#include "hid/display/display.h"

namespace deluge::gui::menu_item {

std::string_view FxPluginParam::getName() const {
	auto const& info = deluge::plugin::fxBankParamInfo(bankIndex_);
	return display->haveOLED() ? info.name : info.shortName;
}

std::string_view FxPluginParam::getTitle() const {
	return deluge::plugin::fxBankParamInfo(bankIndex_).name;
}

} // namespace deluge::gui::menu_item
