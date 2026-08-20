/*
 * Copyright (c) 2014-2024 Synthstrom Audible Limited
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

#include "gui/ui/keyboard/chords.h"
#include "definitions_cxx.hpp"
#include "gui/ui/keyboard/chord_library_file.h"
#include "hid/display/display.h"
#include "io/debug/log.h"
#include "model/settings/runtime_feature_settings.h"
#include "printf.h"
#include <array>
#include <cstring>
#include <stdlib.h>
#include <strings.h>

namespace deluge::gui::ui::keyboard {

ChordQuality getChordQuality(NoteSet& notes) {
	if (notes.count() < 3) {
		return ChordQuality::OTHER;
	}
	else if (notes.has(MAJ3) && notes.has(P5)) {
		if (notes.has(MIN7)) {
			return ChordQuality::DOMINANT;
		}
		else {
			return ChordQuality::MAJOR;
		}
	}
	else if (notes.has(MIN3) && notes.has(P5)) {
		return ChordQuality::MINOR;
	}
	else if (notes.has(MIN3) && notes.has(DIM5)) {
		return ChordQuality::DIMINISHED;
	}
	else if (notes.has(MAJ3) && notes.has(AUG5)) {
		return ChordQuality::AUGMENTED;
	}
	else {
		return ChordQuality::OTHER;
	}
}

ChordLibrary chordLibrary{};

// The one file-backed library. Lives in SDRAM: ~5 KB, only ever touched from the UI.
PLACE_SDRAM_BSS static ChordLibraryData fileLibrary;

const char* const kJazzPageNames[] = {"Chart", "Extensions", "Altered"};

void ChordLibrary::useBuiltIn(ChordLibraryType type) {
	switch (type) {
	case ChordLibraryType::JAZZ:
		chords_ = jazzChordLibrary.data();
		chordCount_ = kJazzLibraryChords;
		break;
	default:
		type = ChordLibraryType::DEFAULT;
		chords_ = defaultChordLibrary.data();
		chordCount_ = kDefaultLibraryChords;
		break;
	}
	library_ = type;
	generation_++;
}

bool ChordLibrary::useFile(char const* name) {
	Error error = readChordLibraryFile(name, fileLibrary, lastError_, sizeof(lastError_));
	if (error != Error::NONE) {
		return false;
	}
	chords_ = fileLibrary.chords;
	chordCount_ = fileLibrary.chordCount;
	library_ = ChordLibraryType::FILE;
	generation_++;
	return true;
}

void ChordLibrary::select(char const* name) {
	lastError_[0] = '\0';
	if (name == nullptr || *name == '\0' || strcasecmp(name, kDefaultChordLibraryName) == 0) {
		name = kDefaultChordLibraryName;
		useBuiltIn(ChordLibraryType::DEFAULT);
	}
	else if (strcasecmp(name, kJazzChordLibraryName) == 0) {
		name = kJazzChordLibraryName;
		useBuiltIn(ChordLibraryType::JAZZ);
	}
	else if (!useFile(name)) {
		// Keep the name the user asked for, so the setting is not silently rewritten, but play the default set
		useBuiltIn(ChordLibraryType::DEFAULT);
	}
	strncpy(name_, name, sizeof(name_) - 1);
	name_[sizeof(name_) - 1] = '\0';
}

void ChordLibrary::selectNext() {
	char next[kMaxChordLibraryNameLength];
	const char* chosen;
	if (library_ == ChordLibraryType::DEFAULT && strcasecmp(name_, kDefaultChordLibraryName) == 0) {
		chosen = kJazzChordLibraryName;
	}
	else {
		// After Jazz the files start from the top; after a file (loaded or not) the next one up alphabetically
		const char* after = (library_ == ChordLibraryType::JAZZ) ? "" : name_;
		chosen = nextChordLibraryFile(after, next) ? next : kDefaultChordLibraryName;
	}
	runtimeFeatureSettings.setChordLibraryName(chosen);
	select(chosen);
}

void ChordLibrary::refreshFromSettings() {
	const char* wanted = runtimeFeatureSettings.getChordLibraryName();
	if (*wanted == '\0') {
		wanted = kDefaultChordLibraryName;
	}
	if (strcasecmp(wanted, name_) != 0) {
		select(wanted);
	}
}

const char* ChordLibrary::pageName(int32_t page) const {
	if (page < 0) {
		return nullptr;
	}
	switch (library_) {
	case ChordLibraryType::JAZZ:
		return (page < (int32_t)(sizeof(kJazzPageNames) / sizeof(kJazzPageNames[0]))) ? kJazzPageNames[page] : nullptr;
	case ChordLibraryType::FILE:
		return (page < kMaxLibraryPages) ? fileLibrary.pageNames[page] : nullptr;
	default:
		return nullptr;
	}
}

void ChordList::refreshFromSettings() {
	chordLibrary.refreshFromSettings();
	if (seenGeneration != chordLibrary.generation()) {
		seenGeneration = chordLibrary.generation();
		chordRowOffset = 0;
		for (int8_t i = 0; i < kUniqueChords; i++) {
			voicingOffset[i] = 0;
		}
	}
}

Voicing ChordList::getChordVoicing(int8_t chordNo) {
	// Check if chord number is valid
	chordNo = validateChordNo(chordNo);

	int8_t voicingNo = voicingOffset[chordNo];
	if (voicingNo <= 0) {
		return chordLibrary.chord(chordNo).voicings[0];
	}
	// Check if voicing is valid
	// voicings after the first should default to 0
	// So if the voicing is all 0, we should return the voicing before it
	else if (voicingNo > 0) {
		// voicingOffset can be set out of bounds (the UI lets it run past the end), so clamp to the last real voicing
		// before indexing - voicings[] only has kUniqueVoicings entries and reading past them is a buffer overflow.
		if (voicingNo >= kUniqueVoicings) {
			voicingNo = kUniqueVoicings - 1;
		}
		for (int voicingN = voicingNo; voicingN >= 0; voicingN--) {
			Voicing voicing = chordLibrary.chord(chordNo).voicings[voicingN];

			bool valid = false;
			for (int j = 0; j < kMaxChordKeyboardSize; j++) {
				if (voicing.offsets[j] != 0) {
					valid = true;
				}
			}
			if (valid) {
				return voicing;
			}
		}
		D_PRINTLN("Voicing is invalid, returning default voicing");
		return chordLibrary.chord(0).voicings[0];
	}
	return chordLibrary.chord(chordNo).voicings[0];
}

void ChordList::adjustChordRowOffset(int8_t offset) {
	int8_t maxRowOffset = std::max<int8_t>(0, chordCount() - kDisplayHeight);
	if (offset > 0) {
		chordRowOffset = std::min<int8_t>(maxRowOffset, chordRowOffset + offset);
	}
	else {
		chordRowOffset = std::max<int8_t>(0, chordRowOffset + offset);
	}
}

void ChordList::adjustVoicingOffset(int8_t chordNo, int8_t offset) {
	// Check if chord number is valid
	chordNo = validateChordNo(chordNo);

	if (offset > 0) {
		voicingOffset[chordNo] = std::min<int8_t>(kUniqueVoicings - 1, voicingOffset[chordNo] + offset);
	}
	else {
		voicingOffset[chordNo] = std::max<int8_t>(0, voicingOffset[chordNo] + offset);
	}
}

int8_t ChordList::validateChordNo(int8_t chordNo) {
	if (chordNo < 0) {
		D_PRINTLN("Chord number is negative, returning chord 0");
		chordNo = 0;
	}
	else if (chordNo >= chordCount()) {
		D_PRINTLN("Chord number is too high, returning last chord");
		chordNo = chordCount() - 1;
	}
	return chordNo;
}
// ChordList
PLACE_SDRAM_DATA const Chord kEmptyChord = {"", NoteSet({ROOT}), {{0, NONE, NONE, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMajor = {"M",
                                       NoteSet({ROOT, MAJ3, P5}),
                                       {{ROOT, MAJ3, P5, NONE, NONE, NONE, NONE},
                                        {ROOT, OCT + MAJ3, P5, NONE, NONE, NONE, NONE},
                                        {ROOT, OCT + MAJ3, P5, -OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor = {"-",
                                       NoteSet({ROOT, MIN3, P5}),
                                       {{ROOT, MIN3, P5, NONE, NONE, NONE, NONE},
                                        {ROOT, OCT + MIN3, P5, NONE, NONE, NONE, NONE},
                                        {ROOT, OCT + MIN3, P5, -OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kDim = {"DIM",
                                     NoteSet({ROOT, MIN3, DIM5}),
                                     {{ROOT, MIN3, DIM5, NONE, NONE, NONE, NONE},
                                      {ROOT, OCT + MIN3, DIM5, NONE, NONE, NONE, NONE},
                                      {ROOT, OCT + MIN3, DIM5, -OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kFullDim = {
    "FULLDIM", NoteSet({ROOT, MIN3, DIM5, DIM7}), {{ROOT, MIN3, DIM5, DIM7, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kAug = {"AUG",
                                     NoteSet({ROOT, MIN3, AUG5}),
                                     {{ROOT, MIN3, AUG5, NONE, NONE, NONE, NONE},
                                      {ROOT, OCT + MIN3, AUG5, NONE, NONE, NONE, NONE},
                                      {ROOT, OCT + MIN3, AUG5, -OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kSus2 = {"SUS2",
                                      NoteSet({ROOT, MAJ2, P5}),
                                      {{ROOT, MAJ2, P5, NONE, NONE, NONE, NONE},
                                       {ROOT, MAJ2 + OCT, P5, NONE, NONE, NONE, NONE},
                                       {ROOT, MAJ2 + OCT, P5, -OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kSus4 = {"SUS4",
                                      NoteSet({ROOT, P4, P5}),
                                      {{ROOT, P4, P5, NONE, NONE, NONE, NONE},
                                       {ROOT, P4 + OCT, P5, NONE, NONE, NONE, NONE},
                                       {ROOT, P4 + OCT, P5, -OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k7 = {"7",
                                   NoteSet({ROOT, MAJ3, P5, MIN7}),
                                   {{ROOT, MAJ3, P5, MIN7, NONE, NONE, NONE},
                                    {ROOT, MAJ3 + OCT, P5, MIN7, NONE, NONE, NONE},
                                    {ROOT, MAJ3 + OCT, P5, MIN7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k7Sus4 = {"7SUS4",
                                       NoteSet({ROOT, P4, P5, MIN7}),
                                       {{ROOT, P4, P5, MIN7, NONE, NONE, NONE},
                                        {ROOT, P4 + OCT, P5, MIN7, NONE, NONE, NONE},
                                        {ROOT, P4 + OCT, P5, MIN7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k7Sus2 = {"7SUS2",
                                       NoteSet({ROOT, MAJ2, P5, MIN7}),
                                       {{ROOT, MAJ2, P5, MIN7, NONE, NONE, NONE},
                                        {ROOT, MAJ2 + OCT, P5, MIN7, NONE, NONE, NONE},
                                        {ROOT, MAJ2 + OCT, P5, MIN7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kM7 = {"M7",
                                    NoteSet({ROOT, MAJ3, P5, MAJ7}),
                                    {{ROOT, MAJ3, P5, MAJ7, NONE, NONE, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MAJ7, NONE, NONE, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MAJ7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor7 = {"-7",
                                        NoteSet({ROOT, MIN3, P5, MIN7}),
                                        {{ROOT, MIN3, P5, MIN7, NONE, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5, MIN7, NONE, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5, MIN7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor2 = {"-2",
                                        NoteSet({ROOT, MIN3, P5, MAJ2}),
                                        {{ROOT, MIN3, P5, MAJ2, NONE, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5, MAJ2, NONE, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5 + OCT, MAJ2, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor4 = {"-4",
                                        NoteSet({ROOT, MIN3, P5, P4}),
                                        {{ROOT, MIN3, P5, P4, NONE, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5, P4, NONE, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5 + OCT, P4, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinorMaj7 = {"-M7",
                                           NoteSet({ROOT, MIN3, P5, MAJ7}),
                                           {{ROOT, MIN3, P5, MAJ7, NONE, NONE, NONE},
                                            {ROOT, MIN3 + OCT, P5, MAJ7, NONE, NONE, NONE},
                                            {ROOT, MIN3 + OCT, P5, MAJ7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor7b5 = {"-7" FLAT_CHAR_STR "5",
                                          NoteSet({ROOT, MIN3, DIM5, MIN7}),
                                          {{ROOT, MIN3, DIM5, MIN7, NONE, NONE, NONE},
                                           {ROOT, MIN3 + OCT, DIM5, MIN7, NONE, NONE, NONE},
                                           {ROOT, MIN3 + OCT, DIM5, MIN7 + OCT, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor9b5 = {
    "-9" FLAT_CHAR_STR "5", NoteSet({ROOT, MIN3, DIM5, MIN7, MAJ2}), {{ROOT, MIN3, DIM5, MIN7, MAJ9, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor7b5b9 = {"-7" FLAT_CHAR_STR "5" FLAT_CHAR_STR "9",
                                            NoteSet({ROOT, MIN3, DIM5, MIN7, MIN2}),
                                            {{ROOT, MIN3, DIM5, MIN7, MIN9, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k9 = {"9",
                                   NoteSet({ROOT, MAJ3, P5, MIN7, MAJ2}),
                                   {{ROOT, MAJ3, P5, MIN7, MAJ9, NONE, NONE},
                                    {ROOT, MAJ3 + OCT, P5, MIN7, MAJ9, NONE, NONE},
                                    {ROOT, MAJ3 + OCT, P5, MIN7 + OCT, MAJ9, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kM9 = {"M9",
                                    NoteSet({ROOT, MAJ3, P5, MAJ7, MAJ2}),
                                    {{ROOT, MAJ3, P5, MAJ7, MAJ9, NONE, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MAJ7, MAJ9, NONE, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MAJ7 + OCT, MAJ9, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor9 = {"-9",
                                        NoteSet({ROOT, MIN3, P5, MIN7, MAJ2}),
                                        {{ROOT, MIN3, P5, MIN7, MAJ9, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5, MIN7, MAJ9, NONE, NONE},
                                         {ROOT, MIN3 + OCT, P5, MIN7 + OCT, MAJ9, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k11 = {"11",
                                    NoteSet({ROOT, MAJ3, P5, MIN7, MAJ2, P4}),
                                    {{ROOT, MAJ3, P5, MIN7, MAJ9, P11, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MIN7, MAJ9, P11, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MIN7 + OCT, MAJ9, P11, NONE}}};
PLACE_SDRAM_DATA const Chord kM11 = {"M11",
                                     NoteSet({ROOT, MAJ3, P5, MAJ7, MAJ2, P4}),
                                     {{ROOT, MAJ3, P5, MAJ7, MAJ9, P11, NONE},
                                      {ROOT, MAJ3 + OCT, P5, MAJ7, MAJ9, P11, NONE},
                                      {ROOT, MAJ3 + OCT, P5, MAJ7 + OCT, MAJ9, P11, NONE}}};
PLACE_SDRAM_DATA const Chord kMinor11 = {"-11",
                                         NoteSet({ROOT, MIN3, P5, MIN7, MAJ2, P4}),
                                         {{ROOT, MIN3, P5, MIN7, MAJ9, P11, NONE},
                                          {{ROOT, P4, MIN7, MIN3 + OCT, P5 + OCT, NONE, NONE}, "SO WHAT"},
                                          {ROOT, MIN3 + OCT, P5, MIN7, MAJ9, P11, NONE},
                                          {ROOT, MIN3 + OCT, P5, MIN7 + OCT, MAJ9, P11, NONE}}};
// 11th are often omitted in 13th and M13th chords because they clash with the major 3rd
// if anything, the 11th is often played as a #11
PLACE_SDRAM_DATA const Chord k13 = {"13",
                                    NoteSet({ROOT, MAJ3, P5, MIN7, MAJ2, MAJ6}),
                                    {{ROOT, MAJ3, P5, MIN7, MAJ9, MAJ13, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MIN7, MAJ9, MAJ13, NONE},
                                     {ROOT, MAJ3 + OCT, P5, MIN7 + OCT, MAJ9, MAJ13, NONE}}};
PLACE_SDRAM_DATA const Chord kM13 = {"M13",
                                     NoteSet({ROOT, MAJ3, P5, MAJ7, MAJ2, MAJ6}),
                                     {{ROOT, MAJ3, P5, MAJ7, MAJ9, MAJ13, NONE},
                                      {ROOT, MAJ3 + OCT, P5, MAJ7, MAJ9, MAJ13, NONE},
                                      {ROOT, MAJ3 + OCT, P5, MAJ7 + OCT, MAJ9, MAJ13, NONE}}};
PLACE_SDRAM_DATA const Chord kM13Sharp11 = {"M13#11",
                                            NoteSet({ROOT, MAJ3, P5, MAJ7, MAJ2, MAJ6, AUG4}),
                                            {{ROOT, MAJ3, P5, MAJ7, MAJ9, MAJ13, AUG11},
                                             {ROOT, MAJ3 + OCT, P5, MAJ7, MAJ9, MAJ13, AUG11},
                                             {ROOT, MAJ3 + OCT, P5, MAJ7 + OCT, MAJ9, MAJ13, AUG11}}};
PLACE_SDRAM_DATA const Chord kMinor13 = {"-13",
                                         NoteSet({ROOT, MIN3, P5, MIN7, MAJ2, P4, MAJ6}),
                                         {{ROOT, MIN3, P5, MIN7, MAJ9, P11, MAJ13},
                                          {ROOT, MIN3 + OCT, P5, MIN7, MAJ9, P11, MAJ13},
                                          {ROOT, MIN3 + OCT, P5, MIN7 + OCT, MAJ9, P11, MAJ13}}};
PLACE_SDRAM_DATA const Chord k6 = {"6",
                                   NoteSet({ROOT, MAJ3, P5, MAJ6}),
                                   {
                                       {ROOT, MAJ3, P5, MAJ6, NONE, NONE, NONE},
                                   }};
PLACE_SDRAM_DATA const Chord k2 = {"2",
                                   NoteSet({ROOT, MAJ3, P5, MAJ2}),
                                   {
                                       {{ROOT, MAJ3 - OCT, P5, MAJ2, NONE, NONE, NONE}, "Open Mu"},
                                       {{ROOT, MAJ3, P5, MAJ2, NONE, NONE, NONE}, "Mu"},
                                   }};
PLACE_SDRAM_DATA const Chord k69 = {"69",
                                    NoteSet({ROOT, MAJ3, P5, MAJ6, MAJ2}),
                                    {
                                        {ROOT, MAJ3, P5, MAJ6, MAJ9, NONE, NONE},
                                    }};
PLACE_SDRAM_DATA const Chord kMinor6 = {"-6",
                                        NoteSet({ROOT, MIN3, P5, MAJ6}),
                                        {
                                            {ROOT, MIN3, P5, MAJ6, NONE, NONE, NONE},
                                        }};

PLACE_SDRAM_DATA const Chord kMinor69 = {
    "-69",
    NoteSet({ROOT, MIN3, P5, MAJ6, MAJ2}),
    {{ROOT, MIN3, P5, MAJ6, MAJ9, NONE, NONE}, {ROOT, MIN3 + OCT, P5, MAJ6, MAJ9, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord kM7Sharp11 = {"M7#11",
                                           NoteSet({ROOT, MAJ3, P5, MAJ7, AUG4}),
                                           {{ROOT, MAJ3, P5, MAJ7, AUG11, NONE, NONE},
                                            {{ROOT, MAJ3, P5, MAJ7, MAJ9, AUG11, NONE}, "ADD 9"},
                                            {ROOT, MAJ3 + OCT, P5, MAJ7, AUG11, NONE, NONE},
                                            {ROOT, MAJ3 + OCT, P5, MAJ7 + OCT, AUG11, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k7b9 = {"7" FLAT_CHAR_STR "9",
                                     NoteSet({ROOT, MAJ3, P5, MIN7, MIN2}),
                                     {{ROOT, MAJ3, P5, MIN7, MIN9, NONE, NONE},
                                      {ROOT, MAJ3 + OCT, P5, MIN7, MIN9, NONE, NONE},
                                      {ROOT, MAJ3 + OCT, P5, MIN7 + OCT, MIN9, NONE, NONE}}};
// The fifth is optional on a 7#9 - dropping it is what gives the "Hendrix" voicing its bite
PLACE_SDRAM_DATA const Chord k7Sharp9 = {"7#9",
                                         NoteSet({ROOT, MAJ3, P5, MIN7, MIN3}),
                                         {{ROOT, MAJ3, P5, MIN7, MIN10, NONE, NONE},
                                          {{ROOT, MAJ3, MIN7, MIN10, NONE, NONE, NONE}, "HENDRIX"},
                                          {ROOT, MAJ3 + OCT, P5, MIN7, MIN10, NONE, NONE},
                                          {ROOT, MAJ3 + OCT, P5, MIN7 + OCT, MIN10, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k7Sharp11 = {"7#11",
                                          NoteSet({ROOT, MAJ3, P5, MIN7, AUG4}),
                                          {{ROOT, MAJ3, P5, MIN7, AUG11, NONE, NONE},
                                           {{ROOT, MAJ3, P5, MIN7, MAJ9, AUG11, NONE}, "ADD 9"},
                                           {ROOT, MAJ3 + OCT, P5, MIN7, AUG11, NONE, NONE},
                                           {ROOT, MAJ3 + OCT, P5, MIN7 + OCT, AUG11, NONE, NONE}}};
// The perfect fifth is left out of the b13 chords - a natural 5 a semitone under the b13 just muddies it
PLACE_SDRAM_DATA const Chord k7b13 = {"7" FLAT_CHAR_STR "13",
                                      NoteSet({ROOT, MAJ3, MIN7, MIN6}),
                                      {{ROOT, MAJ3, MIN7, MIN13, NONE, NONE, NONE},
                                       {{ROOT, MAJ3, MIN7, MAJ9, MIN13, NONE, NONE}, "ADD 9"},
                                       {ROOT, MAJ3 + OCT, MIN7, MIN13, NONE, NONE, NONE}}};
PLACE_SDRAM_DATA const Chord k7Alt = {"7ALT",
                                      NoteSet({ROOT, MAJ3, MIN7, MIN3, MIN6}),
                                      {{ROOT, MAJ3, MIN7, MIN10, MIN13, NONE, NONE},
                                       {{ROOT, MAJ3, MIN7, MIN9, MIN13, NONE, NONE}, FLAT_CHAR_STR "9"},
                                       {{ROOT, MAJ3, MIN7, MIN9, MIN10, MIN13, NONE}, "FULL"},
                                       {ROOT, MAJ3 + OCT, MIN7, MIN10, MIN13, NONE, NONE}}};

// The full library, in the order the chord library keyboard scrolls through it
PLACE_SDRAM_DATA const std::array<const Chord, kDefaultLibraryChords> defaultChordLibrary = {
    kEmptyChord, kMajor,  kMinor,  k6,      k2,   k69,      kSus2,    kSus4,   k7,         k7Sus4,      k7Sus2,
    kM7,         kMinor7, kMinor2, kMinor4, kDim, kFullDim, kAug,     kMinor6, kMinorMaj7, kMinor7b5,   kMinor9b5,
    kMinor7b5b9, k9,      kM9,     kMinor9, k11,  kM11,     kMinor11, k13,     kM13,       kM13Sharp11, kMinor13,
};

// Jazz library: a chord chart. Page one stacks the qualities a lead sheet actually calls for, so a standard is
// played by walking the root columns and picking the row - e.g. Autumn Leaves is -7, 7, maj7, maj7, -7b5, 7b9, -
// without leaving the page. Pages two and three hold the same idea with more colour: extensions, then altered
// and suspended voicings. Rows run bottom to top.
PLACE_SDRAM_DATA const std::array<const Chord, kJazzLibraryChords> jazzChordLibrary = {
    // Chart: triads, the ii-V-I qualities in major and minor, and the diminished passing chord
    kMajor,
    kMinor,
    k7,
    kMinor7,
    kM7,
    kMinor7b5,
    k7b9,
    kFullDim,
    // Extensions: sixths and the ninths, elevenths and thirteenth
    k6,
    k69,
    kMinor6,
    kMinor9,
    k9,
    kM9,
    kMinor11,
    k13,
    // Altered: altered dominants, then the sharp-eleven, minor-major, augmented and suspended colours
    k7Sharp9,
    k7Sharp11,
    k7b13,
    k7Alt,
    kM7Sharp11,
    kMinorMaj7,
    kAug,
    k7Sus4,
};

PLACE_SDRAM_DATA const std::array<const Chord, 10> majorChords = {kMajor, kM7,  k6,    k2,    k69,
                                                                  kM9,    kM13, kSus4, kSus2, kM13Sharp11};

PLACE_SDRAM_DATA const std::array<const Chord, 10> minorChords = {
    kMinor, kMinor7, kMinor4, kMinor11, kMinor6, kMinor2, kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord,
};

PLACE_SDRAM_DATA const std::array<const Chord, 10> dominantChords = {
    kMajor, k7, k69, k9, k7Sus4, k7Sus2, k11, k13, kEmptyChord, kEmptyChord,
};

PLACE_SDRAM_DATA const std::array<const Chord, 10> diminishedChords = {
    kDim,        kMinor7b5,   kMinor7b5b9, kEmptyChord, kEmptyChord,
    kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord,
};

PLACE_SDRAM_DATA const std::array<const Chord, 10> augmentedChords = {
    kAug,        kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord,
    kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord,
};

PLACE_SDRAM_DATA const std::array<const Chord, 10> otherChords = {
    kSus2,       kSus4,       kEmptyChord, kEmptyChord, kEmptyChord,
    kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord, kEmptyChord,
};

} // namespace deluge::gui::ui::keyboard
