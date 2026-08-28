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
#include "gui/menu_item/unpatched_param.h"
#include "plugin/host/fx_plugin_bank.h"

namespace deluge::gui::menu_item {

/// A slot of the insert-FX plugin param bank. Behaves exactly like any UnpatchedParam knob, but is named by the
/// plugin that owns the slot instead of by an l10n string, so a new plugin needs no menu edits.
class FxPluginParam final : public UnpatchedParam {
public:
	explicit FxPluginParam(uint32_t bankIndex)
	    : UnpatchedParam(l10n::String::EMPTY_STRING, deluge::plugin::fxBankParamId(bankIndex), RenderingStyle::BAR),
	      bankIndex_(bankIndex) {}

	[[nodiscard]] std::string_view getName() const override;
	[[nodiscard]] std::string_view getTitle() const override;

private:
	uint32_t bankIndex_;
};

} // namespace deluge::gui::menu_item
