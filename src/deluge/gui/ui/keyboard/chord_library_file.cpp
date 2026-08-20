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

#include "gui/ui/keyboard/chord_library_file.h"
#include "definitions_cxx.hpp"
#include "printf.h"
#include "storage/storage_manager.h"
#include "util/d_string.h"
#include <cstring>
#include <strings.h>

#include "fatfs/ff.h"

namespace deluge::gui::ui::keyboard {

void ChordLibraryData::reset() {
	chordCount = 0;
	poolUsed = 0;
	name = "";
	for (auto& pageName : pageNames) {
		pageName = nullptr;
	}
	for (auto& chord : chords) {
		chord = kEmptyChord;
	}
}

const char* ChordLibraryData::intern(const char* text) {
	size_t length = strlen(text) + 1;
	if (poolUsed + length > kChordLibraryStringPoolSize) {
		return nullptr;
	}
	char* slot = pool + poolUsed;
	memcpy(slot, text, length);
	poolUsed += length;
	return slot;
}

namespace {

struct IntervalName {
	const char* name;
	int8_t semitones;
};

// Same spellings as the constants in chords.h, so a library file reads like the built-in tables
constexpr IntervalName kIntervalNames[] = {
    {"ROOT", ROOT}, {"MIN2", MIN2},   {"MAJ2", MAJ2},   {"MIN3", MIN3},   {"MAJ3", MAJ3},   {"P4", P4},
    {"AUG4", AUG4}, {"DIM5", DIM5},   {"P5", P5},       {"AUG5", AUG5},   {"MIN6", MIN6},   {"MAJ6", MAJ6},
    {"DIM7", DIM7}, {"MIN7", MIN7},   {"DOM7", DOM7},   {"MAJ7", MAJ7},   {"OCT", OCT},     {"MIN9", MIN9},
    {"MAJ9", MAJ9}, {"MIN10", MIN10}, {"MAJ10", MAJ10}, {"P11", P11},     {"AUG11", AUG11}, {"DIM12", DIM12},
    {"P12", P12},   {"MIN13", MIN13}, {"MAJ13", MAJ13}, {"MIN14", MIN14}, {"MAJ14", MAJ14},
};

void setError(char* errorText, size_t errorTextLength, const char* message) {
	if (errorText != nullptr && errorTextLength > 0) {
		strncpy(errorText, message, errorTextLength - 1);
		errorText[errorTextLength - 1] = '\0';
	}
}

bool parseIntervalMagnitude(const char* token, size_t length, int8_t& semitones);

/// One token of a notes list -> semitones from the root. Returns false if it is not an interval.
bool parseInterval(const char* token, size_t length, int8_t& semitones) {
	// A leading minus drops the note below the root, as the built-in "-OCT" bass voicings do
	bool negative = false;
	if (length > 1 && token[0] == '-') {
		negative = true;
		token++;
		length--;
	}
	if (!parseIntervalMagnitude(token, length, semitones)) {
		return false;
	}
	if (negative) {
		semitones = -semitones;
	}
	return true;
}

bool parseIntervalMagnitude(const char* token, size_t length, int8_t& semitones) {
	for (const IntervalName& interval : kIntervalNames) {
		if (strlen(interval.name) == length && strncasecmp(interval.name, token, length) == 0) {
			semitones = interval.semitones;
			return true;
		}
	}
	// Plain semitone count. Anything a seven note voicing could reach from a bottom note of kMaxBottomNote.
	int32_t value = 0;
	if (length == 0 || length > 2) {
		return false;
	}
	for (size_t i = 0; i < length; i++) {
		if (token[i] < '0' || token[i] > '9') {
			return false;
		}
		value = value * 10 + (token[i] - '0');
	}
	if (value > 36) {
		return false;
	}
	semitones = value;
	return true;
}

/// "ROOT, MIN3 P5" -> voicing offsets, padded with NONE
bool parseNotes(const char* text, Voicing& voicing) {
	for (auto& offset : voicing.offsets) {
		offset = NONE;
	}
	int32_t count = 0;
	const char* p = text;
	while (*p != '\0') {
		while (*p == ' ' || *p == ',' || *p == '\t') {
			p++;
		}
		if (*p == '\0') {
			break;
		}
		const char* start = p;
		while (*p != '\0' && *p != ' ' && *p != ',' && *p != '\t') {
			p++;
		}
		if (count >= kMaxChordKeyboardSize) {
			return false;
		}
		if (!parseInterval(start, p - start, voicing.offsets[count])) {
			return false;
		}
		count++;
	}
	return count > 0;
}

/// Turn the "b" people type into the flat glyph the display has, and accept the UTF-8 flat sign too
void convertFlats(String& text) {
	const char* in = text.get();
	char out[64];
	size_t o = 0;
	for (size_t i = 0; in[i] != '\0' && o < sizeof(out) - 1; i++) {
		unsigned char c = in[i];
		if (c == 'b' && in[i + 1] >= '0' && in[i + 1] <= '9') {
			out[o++] = FLAT_CHAR_STR[0];
		}
		else if (c == 0xE2 && (unsigned char)in[i + 1] == 0x99 && (unsigned char)in[i + 2] == 0xAD) {
			out[o++] = FLAT_CHAR_STR[0];
			i += 2;
		}
		else {
			out[o++] = c;
		}
	}
	out[o] = '\0';
	text.set(out);
}

NoteSet intervalSetFromVoicing(const Voicing& voicing) {
	NoteSet set;
	for (int8_t offset : voicing.offsets) {
		if (offset != NONE) {
			set.add(((offset % kOctaveSize) + kOctaveSize) % kOctaveSize);
		}
	}
	return set;
}

bool isLibraryFile(const FILINFO& info, char* nameOut) {
	if (info.fattrib & (AM_DIR | AM_HID | AM_SYS)) {
		return false;
	}
	const char* fileName = info.fname;
	if (fileName[0] == '.' || fileName[0] == '_') {
		return false;
	}
	size_t length = strlen(fileName);
	if (length < 5 || length - 4 >= kMaxChordLibraryNameLength || strcasecmp(fileName + length - 4, ".XML") != 0) {
		return false;
	}
	memcpy(nameOut, fileName, length - 4);
	nameOut[length - 4] = '\0';
	return true;
}

} // namespace

Error readChordLibraryFile(char const* name, ChordLibraryData& data, char* errorText, size_t errorTextLength) {
	data.reset();

	char path[kMaxChordLibraryNameLength + 16];
	snprintf(path, sizeof(path), "%s/%s.XML", kChordLibraryFolder, name);

	FilePointer fp;
	if (!StorageManager::fileExists(path, &fp)) {
		setError(errorText, errorTextLength, "File not found");
		return Error::FILE_NOT_FOUND;
	}
	Error error = StorageManager::openXMLFile(&fp, smDeserializer, "chordLibrary");
	if (error != Error::NONE) {
		setError(errorText, errorTextLength, "Not a chordLibrary file");
		return error;
	}

	Deserializer& reader = *activeDeserializer;
	String text;
	error = Error::NONE;
	const char* problem = nullptr;
	int8_t pageCount = 0;
	char const* tag;

	while (problem == nullptr && *(tag = reader.readNextTagOrAttributeName())) {
		if (strcmp(tag, "name") == 0) {
			reader.readTagOrAttributeValueString(&text);
			data.name = data.intern(text.get());
			if (data.name == nullptr) {
				problem = "Too much text";
			}
		}
		else if (strcmp(tag, "page") == 0) {
			// Every page starts on a fresh screen, so pad the previous one out to a multiple of the screen height
			while (data.chordCount % kDisplayHeight != 0) {
				data.chords[data.chordCount++] = kEmptyChord;
			}
			if (pageCount >= kMaxLibraryPages) {
				problem = "Too many pages";
				break;
			}
			int8_t pageIndex = pageCount++;
			int32_t chordsOnPage = 0;

			while (problem == nullptr && *(tag = reader.readNextTagOrAttributeName())) {
				if (strcmp(tag, "name") == 0) {
					reader.readTagOrAttributeValueString(&text);
					data.pageNames[pageIndex] = data.intern(text.get());
					if (data.pageNames[pageIndex] == nullptr) {
						problem = "Too much text";
					}
				}
				else if (strcmp(tag, "chord") == 0) {
					if (chordsOnPage >= kDisplayHeight) {
						problem = "More than 8 chords on a page";
						break;
					}
					Chord& chord = data.chords[data.chordCount];
					chord = kEmptyChord;
					int32_t voicingCount = 0;

					while (problem == nullptr && *(tag = reader.readNextTagOrAttributeName())) {
						if (strcmp(tag, "name") == 0) {
							reader.readTagOrAttributeValueString(&text);
							convertFlats(text);
							chord.name = data.intern(text.get());
							if (chord.name == nullptr) {
								problem = "Too much text";
							}
						}
						else if (strcmp(tag, "voicing") == 0) {
							if (voicingCount >= kUniqueVoicings) {
								problem = "More than 4 voicings";
								break;
							}
							Voicing& voicing = chord.voicings[voicingCount];
							bool haveNotes = false;

							while (problem == nullptr && *(tag = reader.readNextTagOrAttributeName())) {
								if (strcmp(tag, "name") == 0) {
									reader.readTagOrAttributeValueString(&text);
									convertFlats(text);
									voicing.supplementalName = data.intern(text.get());
									if (voicing.supplementalName == nullptr) {
										problem = "Too much text";
									}
								}
								else if (strcmp(tag, "notes") == 0) {
									reader.readTagOrAttributeValueString(&text);
									if (!parseNotes(text.get(), voicing)) {
										problem = "Bad notes list";
									}
									haveNotes = true;
								}
								reader.exitTag(tag);
							}
							if (problem == nullptr && !haveNotes) {
								problem = "Voicing without notes";
							}
							voicingCount++;
						}
						reader.exitTag(tag);
					}
					// A chord with no voicings is a blank spacer row, like the first row of the default set
					chord.intervalSet = intervalSetFromVoicing(chord.voicings[0]);
					data.chordCount++;
					chordsOnPage++;
				}
				reader.exitTag(tag);
			}
		}
		reader.exitTag(tag);
	}
	smDeserializer.closeWriter();

	if (problem == nullptr && data.chordCount == 0) {
		problem = "No chords";
	}
	if (problem != nullptr) {
		setError(errorText, errorTextLength, problem);
		return Error::FILE_CORRUPTED;
	}
	return Error::NONE;
}

bool nextChordLibraryFile(char const* current, char* next) {
	DIR dir;
	if (f_opendir(&dir, kChordLibraryFolder) != FR_OK) {
		return false;
	}
	// FatFs hands files back in on-disk order, so take one pass and keep the smallest name above `current`
	bool found = false;
	FILINFO info;
	char candidate[kMaxChordLibraryNameLength];
	while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
		if (!isLibraryFile(info, candidate)) {
			continue;
		}
		if (strcasecmp(candidate, current) <= 0) {
			continue;
		}
		if (!found || strcasecmp(candidate, next) < 0) {
			strcpy(next, candidate);
			found = true;
		}
	}
	f_closedir(&dir);
	return found;
}

} // namespace deluge::gui::ui::keyboard
