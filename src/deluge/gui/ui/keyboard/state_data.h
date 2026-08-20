/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
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
#include "gui/ui/keyboard/chords.h"
#include "gui/ui/keyboard/layout/column_control_state.h"
#include "storage/flash_storage.h"

namespace deluge::gui::ui::keyboard {

constexpr int32_t kDefaultIsometricRowInterval = 5;
struct KeyboardStateIsomorphic {
	int32_t scrollOffset = (60 - (kDisplayHeight >> 2) * kDefaultIsometricRowInterval);
	int32_t rowInterval = kDefaultIsometricRowInterval;
};

struct KeyboardStateDrums {
	int32_t scroll_offset = 0;
	int32_t zoom_level = 8;
};

constexpr int32_t kDefaultInKeyRowInterval = 3;
struct KeyboardStateInKey {
	// Init scales have 7 elements, multipled by three octaves gives us C1 as first pad
	int32_t scrollOffset = (7 * 3);
	int32_t rowInterval = kDefaultInKeyRowInterval;
};

struct KeyboardStatePiano {
	// default octave = 1 (0 = -2oct), use a vertical scroll to change it
	int32_t scrollOffset = 3;
	// default note=0 (C)
	int32_t noteOffset = 0;
};

struct KeyboardStateChordLibrary {
	int32_t rowInterval = kOctaveSize;
	int32_t scrollOffset = 0;
	int32_t noteOffset = (rowInterval * 4);
	int32_t rowColorMultiplier = 5;
	/// Columns step through the scale rather than chromatically. Only has an effect in scale mode.
	bool scaleDegreeColumns = false;
	/// Rows become chord functions built from the scale rather than fixed qualities. Only in scale mode.
	bool diatonicQuality = false;
	/// Scroll position for scaleDegreeColumns, counted in scale steps rather than semitones
	int32_t degreeOffset = 0;
	/// Root note that noteOffset currently reflects, so a later key change can shift the grid to match.
	/// Negative until the first render, which is when the song's key first becomes visible.
	int32_t appliedRootNote = -1;
	/// Play single notes on an isomorphic grid instead of chords, for dropping into a lead line.
	/// Deliberately shares KeyboardStateIsomorphic, so it is the same surface as the isomorphic layout.
	bool leadMode = false;
	/// Split the grid: the left hand plays the chord rows on scale-degree columns, the right hand plays single
	/// notes on a grid tuned in fourths. Exclusive with leadMode.
	bool splitMode = false;
	/// Bottom-left note of the split mode's fourths grid. Shifted to the song's key on first render like noteOffset.
	int32_t splitLeadOffset = (kOctaveSize * 4);
	ChordList chordList{};
};

struct KeyboardStateChord {
	int32_t noteOffset = (kOctaveSize * 4);
	int32_t modOffset = 0;
	int32_t scaleOffset = 0;
	bool autoVoiceLeading = false;
};
/// Please note that saving and restoring currently needs to be added manually in instrument_clip.cpp and all layouts
/// share one struct for storage
struct KeyboardState {
	KeyboardLayoutType currentLayout = FlashStorage::defaultKeyboardLayout;

	KeyboardStateIsomorphic isomorphic;
	KeyboardStateDrums drums;
	KeyboardStateInKey inKey;
	KeyboardStatePiano piano;
	KeyboardStateChord chord;
	KeyboardStateChordLibrary chordLibrary;

	layout::ColumnControlState columnControl;
};

}; // namespace deluge::gui::ui::keyboard
