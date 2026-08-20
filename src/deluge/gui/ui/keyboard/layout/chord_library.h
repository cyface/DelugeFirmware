/*
 * Copyright © 2016-2024 Synthstrom Audible Limited
 * Copyright © 2024 Madeline Scyphers
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

#include "definitions.h"
#include "gui/ui/keyboard/chords.h"
#include "gui/ui/keyboard/layout/column_controls.h"
#include <array>

namespace deluge::gui::ui::keyboard::layout {

constexpr int8_t kVerticalPages = ((kUniqueChords + kDisplayHeight - 1) / kDisplayHeight); // Round up division

/// The two rightmost grid columns are control strips, so only the columns to their left play chords.
constexpr int32_t kControlColumn = kDisplayWidth - 1; // modes at the top, play controls at the bottom
constexpr int32_t kPageColumn = kDisplayWidth - 2;    // one pad per page of the active library
constexpr int32_t kChordLibraryColumns = kPageColumn;

/// Control rows, as firmware row indices - row 0 is the BOTTOM of the display, matching the chord rows
/// themselves. The gap at rows 3 and 4 separates the mode group from the play group.
constexpr int32_t kControlRowLead = 0;
constexpr int32_t kControlRowOctaveDown = 1;
constexpr int32_t kControlRowOctaveUp = 2;
constexpr int32_t kControlRowScaleDegree = 5;
constexpr int32_t kControlRowDiatonic = 6;
constexpr int32_t kControlRowLibrary = 7;
static_assert(kVerticalPages <= kDisplayHeight, "Page column needs one row per page");

/// Keep the bottom-left note low enough that a seven note voicing on top of it still fits in MIDI range
constexpr int32_t kMaxBottomNote = 96;

/// @brief Represents a keyboard layout for chord-based input.
class KeyboardLayoutChordLibrary : public ColumnControlsKeyboard {
public:
	KeyboardLayoutChordLibrary() = default;
	~KeyboardLayoutChordLibrary() override = default;

	void evaluatePads(PressedPad presses[kMaxNumKeyboardPadPresses]) override;
	void handleVerticalEncoder(int32_t offset) override;
	void handleHorizontalEncoder(int32_t offset, bool shiftEnabled, PressedPad presses[kMaxNumKeyboardPadPresses],
	                             bool encoderPressed = false) override;
	void precalculate() override;

	void renderPads(RGB image[][kDisplayWidth + kSideBarWidth]) override;

	l10n::String name() override { return l10n::String::STRING_FOR_KEYBOARD_LAYOUT_CHORD_LIBRARY; }
	bool supportsInstrument() override { return true; }
	bool supportsKit() override { return false; }

protected:
	bool allowSidebarType(ColumnControlFunction sidebarType) override;

private:
	void drawChordName(int16_t noteCode, const char* chordName = "", const char* voicingName = "");

	/// True when the scale-aware modes are actually in effect - they need a scale to work from
	bool scaleModesActive() { return getScaleModeEnabled(); }
	bool usingScaleDegrees() { return scaleModesActive() && getState().chordLibrary.scaleDegreeColumns; }
	bool usingDiatonicQuality() {
		return scaleModesActive() && getState().chordLibrary.diatonicQuality && !getState().chordLibrary.leadMode;
	}
	bool usingLeadMode() { return getState().chordLibrary.leadMode; }

	/// Note under a grid pad while playing lead, on the same isomorphic grid as the isomorphic layout
	uint8_t noteFromLeadCoords(int32_t x, int32_t y);
	/// Move whatever the active mode scrolls by a whole octave
	void shiftOctave(int32_t direction);
	/// Lowest note currently reachable, used to report where an octave shift landed
	int32_t bottomNote();

	/// Root note under a grid column, either chromatic or stepping through the scale
	uint8_t noteFromCoords(int32_t x);
	/// Absolute note for a position counted in scale steps from the root note
	int32_t noteFromDegreeIndex(int32_t degreeIndex);
	/// Nearest scale-step position at or below a note, used to keep the two column modes in sync
	int32_t degreeIndexFromNote(int32_t note);

	/// Builds the diatonic chord for a shape rooted on a column, returning how many notes it wrote
	int32_t buildDiatonicChord(int32_t x, int32_t shapeNo, int32_t notes[kMaxChordKeyboardSize]);
	/// Name for a set of intervals, looked up across both libraries, or nullptr if nothing matches
	const char* nameForIntervals(NoteSet intervals);

	void handleControlPad(int32_t x, int32_t y);
	/// Says what a control pad currently is, so holding one describes it and releasing confirms the change
	void popupControlState(int32_t x, int32_t y);
	void popupPage(int32_t page);
	void popupOctave();
	void popupLibraryName();
	void renderControlColumn(RGB image[][kDisplayWidth + kSideBarWidth]);
	int32_t pageCount();

	inline int32_t getChordNo(int32_t y) { return getState().chordLibrary.chordList.chordRowOffset + y; }

	std::array<RGB, kVerticalPages> pageColours;
	/// Which control pad the held-pad description is currently showing, so it is emitted once per press
	int8_t lastDescribedX = -1;
	int8_t lastDescribedY = -1;
};

}; // namespace deluge::gui::ui::keyboard::layout
