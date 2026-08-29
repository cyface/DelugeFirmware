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
#include "gui/menu_item/menu_item.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "plugin/host/plugin_loader.h"
#include "util/functions.h"
#include <cstring>

namespace deluge::gui::menu_item::plugin {

/// Read-only: what the boot scan of PLUGINS/ did with each file on the card. Until plugins can be managed from
/// here (fork issue #41 step 4), this is the only place the user can see whether the plugin they copied on is the
/// one making the sound - and, when it is not, why not.
class Status final : public MenuItem {
public:
	using MenuItem::MenuItem;

	void drawPixelsForOled() override {
		deluge::hid::display::oled_canvas::Canvas& canvas = hid::display::OLED::main;
		std::span<const deluge::plugin::PluginLoadRecord> report = deluge::plugin::pluginLoadReport();
		if (report.empty()) {
			canvas.drawStringCentredShrinkIfNecessary("No plugins on card", 22, 18, 20);
			return;
		}
		int32_t yPixel = OLED_MAIN_TOPMOST_PIXEL + 15;
		// How many files fit, and whether the last line has to be spent saying that some did not: a list that
		// silently stops is how someone ends up sure their plugin was never seen.
		size_t lines = static_cast<size_t>((OLED_MAIN_HEIGHT_PIXELS - yPixel) / kTextSpacingY);
		size_t shown = report.size() <= lines ? report.size() : (lines > 0 ? lines - 1 : 0);
		for (const deluge::plugin::PluginLoadRecord& record : report.first(shown)) {
			// Two columns rather than one run-on line: 128 pixels is about 21 characters, and "plaits_drums.dlp"
			// alone is 16 of them. The status goes hard against the right edge - it is what you came to read -
			// and the name gets whatever is left, clipped rather than pushing the status off the screen.
			const char* status = deluge::plugin::describe(record.status);
			char subject[sizeof(record.file)];
			deluge::plugin::describeSubject(record, subject, sizeof(subject));
			int32_t statusWidth = canvas.getStringWidthInPixels(status, kTextSpacingY);
			// Two characters of margin, not one: drawString's endX is where it stops *starting* characters, so a
			// clipped name can still paint one character's width past it and touch the status.
			canvas.drawString(subject, kTextSpacingX, yPixel, kTextSpacingX, kTextSpacingY, 0,
			                  OLED_MAIN_WIDTH_PIXELS - statusWidth - 2 * kTextSpacingX);
			canvas.drawStringAlignRight(status, yPixel, kTextSpacingX, kTextSpacingY);
			yPixel += kTextSpacingY;
		}
		if (shown < report.size()) {
			char more[24] = "and ";
			char count[12];
			intToString(static_cast<int32_t>(report.size() - shown), count, 1);
			strncat(more, count, sizeof(more) - strlen(more) - 1);
			strncat(more, " more", sizeof(more) - strlen(more) - 1);
			canvas.drawString(more, kTextSpacingX, yPixel, kTextSpacingX, kTextSpacingY);
		}
	}

	void beginSession(MenuItem* navigatedBackwardFrom) override { drawValue(); }

	void drawValue() {
		char summary[64];
		summarise(summary, sizeof(summary));
		display->setScrollingText(summary);
	}

private:
	/// "Drum loaded", or the file name when the file never got far enough to name a plugin. For the 7-segment
	/// display, which scrolls the whole thing rather than laying it out in columns.
	static void describeRecord(const deluge::plugin::PluginLoadRecord& record, char* out, size_t size) {
		deluge::plugin::describeSubject(record, out, size);
		strncat(out, " ", size - strlen(out) - 1);
		strncat(out, deluge::plugin::describe(record.status), size - strlen(out) - 1);
	}

	/// The same thing in one line, for the 7-segment display: what went wrong if anything did, otherwise a count.
	static void summarise(char* out, size_t size) {
		std::span<const deluge::plugin::PluginLoadRecord> report = deluge::plugin::pluginLoadReport();
		out[0] = 0;
		uint32_t loaded = 0;
		for (const deluge::plugin::PluginLoadRecord& record : report) {
			if (record.status == deluge::plugin::PluginLoadStatus::loaded) {
				loaded++;
			}
			else {
				describeRecord(record, out, size);
				return; // the first thing that is not simply working is what the user needs to hear about
			}
		}
		if (loaded == 0) {
			strncat(out, "NONE", size - 1);
			return;
		}
		char count[12];
		intToString(static_cast<int32_t>(loaded), count, 1);
		strncat(out, count, size - 1);
		strncat(out, " LOADED", size - strlen(out) - 1);
	}
};

} // namespace deluge::gui::menu_item::plugin
