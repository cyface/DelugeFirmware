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

#include "definitions_cxx.hpp"
#include "model/scale/note_set.h"
#include <array>
// #include <vector>

constexpr int8_t kMaxChordKeyboardSize = 7;
constexpr int8_t kUniqueVoicings = 4;
// Size of each built-in chord library
constexpr int8_t kDefaultLibraryChords = 33;
constexpr int8_t kJazzLibraryChords = 24;
// Most chords any library can hold, built-in or read from the card: eight pages of eight rows
constexpr int8_t kUniqueChords = 64;
constexpr int8_t kOffScreenChords = kUniqueChords - kDisplayHeight;

namespace deluge::gui::ui::keyboard {

enum class ChordQuality {
	MAJOR,
	MINOR,
	DIMINISHED,
	AUGMENTED,
	DOMINANT,
	OTHER,
	CHORD_QUALITY_MAX,
};

// Check and return the quality of a chord, assuming the notes are defined from the root, even if it is a rootless chord
ChordQuality getChordQuality(NoteSet& notes);

/// @brief Which set of chords the chord library keyboard offers. Selected by the ChordLibrary community feature.
enum class ChordLibraryType : uint8_t {
	DEFAULT = 0,
	JAZZ = 1,
	FILE = 2, ///< Read from CHORDS/<name>.XML on the card
};

/// Names the built-in libraries answer to. Anything else selects a file in the CHORDS folder.
constexpr char const* kDefaultChordLibraryName = "Default";
constexpr char const* kJazzChordLibraryName = "Jazz";

// Interval offsets for convenience
const int8_t NONE = INT8_MAX;
const int8_t ROOT = 0;
const int8_t MIN2 = 1;
const int8_t MAJ2 = 2;
const int8_t MIN3 = 3;
const int8_t MAJ3 = 4;
const int8_t P4 = 5;
const int8_t AUG4 = 6;
const int8_t DIM5 = 6;
const int8_t P5 = 7;
const int8_t AUG5 = 8;
const int8_t MIN6 = 8;
const int8_t MAJ6 = 9;
const int8_t DIM7 = 9;
const int8_t MIN7 = 10;
const int8_t DOM7 = 10;
const int8_t MAJ7 = 11;
const int8_t OCT = kOctaveSize;
const int8_t MIN9 = MIN2 + OCT;
const int8_t MAJ9 = MAJ2 + OCT;
const int8_t MIN10 = MIN3 + OCT;
const int8_t MAJ10 = MAJ3 + OCT;
const int8_t P11 = P4 + OCT;
const int8_t AUG11 = AUG4 + OCT;
const int8_t DIM12 = DIM5 + OCT;
const int8_t P12 = P5 + OCT;
const int8_t MIN13 = MIN6 + OCT;
const int8_t MAJ13 = MAJ6 + OCT;
const int8_t MIN14 = MIN7 + OCT;
const int8_t MAJ14 = MAJ7 + OCT;

/// @brief A voicing is a set of offsets from the root note of a chord
struct Voicing {
	int8_t offsets[kMaxChordKeyboardSize];
	const char* supplementalName = "";
};

/// @brief A chord is a name and a set of voicings
struct Chord {
	const char* name;
	NoteSet intervalSet;
	Voicing voicings[kUniqueVoicings] = {0};
};

// ChordList
extern const Chord kEmptyChord;
extern const Chord kMajor;
extern const Chord kMinor;
extern const Chord k6;
extern const Chord k2;
extern const Chord k69;
extern const Chord kSus2;
extern const Chord kSus4;
extern const Chord k7;
extern const Chord k7Sus4;
extern const Chord k7Sus2;
extern const Chord kM7;
extern const Chord kMinor7;
extern const Chord kMinor2;
extern const Chord kMinor4;
extern const Chord kDim;
extern const Chord kFullDim;
extern const Chord kAug;
extern const Chord kMinor6;
extern const Chord kMinorMaj7;
extern const Chord kMinor7b5;
extern const Chord kMinor9b5;
extern const Chord kMinor7b5b9;
extern const Chord k9;
extern const Chord kM9;
extern const Chord kMinor9;
extern const Chord k11;
extern const Chord kM11;
extern const Chord kMinor11;
extern const Chord k13;
extern const Chord kM13;
extern const Chord kM13Sharp11;
extern const Chord kMinor13;
extern const Chord kMinor69;
extern const Chord kM7Sharp11;
extern const Chord k7b9;
extern const Chord k7Sharp9;
extern const Chord k7Sharp11;
extern const Chord k7b13;
extern const Chord k7Alt;

/// The chord sets a ChordList can be loaded with
extern const std::array<const Chord, kDefaultLibraryChords> defaultChordLibrary;
extern const std::array<const Chord, kJazzLibraryChords> jazzChordLibrary;

extern const std::array<const Chord, 10> majorChords;

extern const std::array<const Chord, 10> minorChords;

extern const std::array<const Chord, 10> dominantChords;

extern const std::array<const Chord, 10> diminishedChords;

extern const std::array<const Chord, 10> augmentedChords;

extern const std::array<const Chord, 10> otherChords;

/// @brief The chord table every clip's chord library keyboard reads from.
///
/// There is exactly one of these (`chordLibrary`), shared by every clip. It only points at the active chord set,
/// so switching sets costs no RAM per clip - the per-clip state (scroll position, chosen voicings) lives in
/// ChordList. `generation` bumps on every swap so ChordLists can tell their offsets have gone stale, even if a
/// reload lands on the same set again.
///
/// A library is selected by name: "Default" and "Jazz" are built in, anything else is CHORDS/<name>.XML on the
/// card. The selected name is remembered in the community feature settings.
class ChordLibrary {
public:
	ChordLibrary() = default;

	/// Select a library by name. A file that cannot be read falls back to the default set and remembers the
	/// problem (see lastError()), so a typo in a file can never leave the keyboard without chords.
	void select(char const* name);
	/// Move to the next library: Default, Jazz, then the CHORDS folder in alphabetical order, wrapping round
	void selectNext();
	/// Make the selection match the community feature setting, if it has changed since last time
	void refreshFromSettings();

	const Chord& chord(int8_t chordNo) const { return chords_[chordNo]; }
	int8_t chordCount() const { return chordCount_; }
	ChordLibraryType library() const { return library_; }
	uint8_t generation() const { return generation_; }
	/// Name of the selected library, as chosen (so a file's name even if it failed to load)
	const char* name() const { return name_; }
	/// Label for a page of the active library, or nullptr if it has none
	const char* pageName(int32_t page) const;
	/// Why the selected file could not be used, or nullptr if the selection loaded fine
	const char* lastError() const { return lastError_[0] ? lastError_ : nullptr; }

private:
	void useBuiltIn(ChordLibraryType type);
	bool useFile(char const* name);

	// Starts on the built-in default so the table is never empty, even before the settings have been read
	const Chord* chords_ = defaultChordLibrary.data();
	int8_t chordCount_ = kDefaultLibraryChords;
	ChordLibraryType library_ = ChordLibraryType::DEFAULT;
	uint8_t generation_ = 0;
	char name_[32] = "Default";
	char lastError_[32] = "";
};

extern ChordLibrary chordLibrary;

/// @brief A clip's view onto the shared chord library: which page it is scrolled to and which voicing of each
/// chord it has chosen. Holds no chord data of its own.
class ChordList {
public:
	ChordList() = default;

	/**
	 * @brief Get a voicing for a chord with a given index. If the voicingOffset for that Chord is out of bounds,
	 * return the max or min voicing depending on the direction.
	 *
	 * @param chordNo The index of the chord
	 * @return The voicing
	 */
	Voicing getChordVoicing(int8_t chordNo);
	void adjustChordRowOffset(int8_t offset);
	void adjustVoicingOffset(int8_t chordNo, int8_t offset);

	/// Make sure the shared library matches the community feature setting, and forget this clip's row/voicing
	/// selections if the library has changed since they were made - they index into a set that is gone.
	void refreshFromSettings();

	const Chord& chord(int8_t chordNo) { return chordLibrary.chord(validateChordNo(chordNo)); }
	int8_t chordCount() const { return chordLibrary.chordCount(); }
	ChordLibraryType library() const { return chordLibrary.library(); }

	int8_t voicingOffset[kUniqueChords] = {0};
	uint8_t chordRowOffset = 0;

private:
	int8_t validateChordNo(int8_t chordNo);
	/// Library generation the offsets above were made against
	uint8_t seenGeneration = 0;
};

} // namespace deluge::gui::ui::keyboard
