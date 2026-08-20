/*
 * Copyright © 2024 Synthstrom Audible Limited
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

#include "gui/ui/keyboard/chords.h"
#include "util/misc.h"
#include <cstddef>
#include <cstdint>

namespace deluge::gui::ui::keyboard {

/// Folder on the card that holds user chord libraries, one <name>.XML per library
constexpr char const* kChordLibraryFolder = "CHORDS";
constexpr int8_t kMaxLibraryPages = kUniqueChords / kDisplayHeight;
/// Longest library name (the file name without its extension) we keep track of, including the terminator
constexpr size_t kMaxChordLibraryNameLength = 32;
/// Backing store for every chord, voicing, page and library name read from one file
constexpr size_t kChordLibraryStringPoolSize = 1536;

/// @brief A chord library read from the card. Chord and Voicing hold their names as plain pointers, so the
/// strings live in an owned pool that stays put for as long as the library is in use.
struct ChordLibraryData {
	Chord chords[kUniqueChords];
	int8_t chordCount = 0;
	const char* pageNames[kMaxLibraryPages] = {};
	const char* name = "";
	char pool[kChordLibraryStringPoolSize];
	size_t poolUsed = 0;

	void reset();
	/// Copy a string into the pool. Returns nullptr when the pool is full.
	const char* intern(const char* text);
};

/**
 * @brief Read CHORDS/<name>.XML into data.
 *
 * Format:
 *   <chordLibrary name="Jazz">
 *     <page name="Minor">
 *       <chord name="-7">
 *         <voicing notes="ROOT, MIN3, P5, MIN7" />
 *         <voicing name="SO WHAT" notes="ROOT, P4, MIN7, MIN10, P12" />
 *       </chord>
 *     </page>
 *   </chordLibrary>
 *
 * Notes are the interval mnemonics from chords.h (ROOT, MIN3, P5, MAJ9 ...) or plain semitone counts, may be
 * separated by commas or spaces, and take a leading minus to go below the root ("-OCT" for a bass note). Each page
 * starts on a fresh screen of eight rows. A chord's highlighting interval set is derived from its first voicing. In
 * names, a 'b' directly before a digit is shown as a flat.
 *
 * On any error data is left in an unspecified state and should not be used; errorText (if given) receives a
 * short description of the problem.
 */
Error readChordLibraryFile(char const* name, ChordLibraryData& data, char* errorText, size_t errorTextLength);

/**
 * @brief Find the library file that follows `current` in case-insensitive alphabetical order.
 *
 * Pass an empty `current` to get the first file. Returns false when there is no later file (or no folder).
 * `next` must have room for kMaxChordLibraryNameLength characters.
 */
bool nextChordLibraryFile(char const* current, char* next);

} // namespace deluge::gui::ui::keyboard
