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

#include "gui/ui/keyboard/layout/chord_library.h"
#include "gui/colour/colour.h"
#include "gui/colour/palette.h"
#include "gui/ui/audio_recorder.h"
#include "gui/ui/browser/sample_browser.h"
#include "gui/ui/keyboard/chords.h"
#include "gui/ui/keyboard/keyboard_screen.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "io/debug/log.h"
#include "model/settings/runtime_feature_settings.h"
#include "util/functions.h"
#include <cmath>
#include <stdlib.h>

namespace deluge::gui::ui::keyboard::layout {

/// A chord shape expressed in scale steps from the chord root, so it stays diatonic in any scale
struct DiatonicShape {
	const char* name;
	int8_t degrees[kMaxChordKeyboardSize]; // NONE-terminated
};

// One shape per grid row. The bottom row is the plain triad, stacking further thirds as you go up, with
// three handy non-tertian shapes on top. Constant, so it stays in rodata rather than SDRAM.
const std::array<DiatonicShape, kDisplayHeight> diatonicShapes = {{
    {"TRIAD", {0, 2, 4, NONE, NONE, NONE, NONE}},
    {"7TH", {0, 2, 4, 6, NONE, NONE, NONE}},
    {"9TH", {0, 2, 4, 6, 8, NONE, NONE}},
    {"11TH", {0, 2, 4, 6, 8, 10, NONE}},
    {"13TH", {0, 2, 4, 6, 8, 10, 12}},
    {"6TH", {0, 2, 4, 5, NONE, NONE, NONE}},
    {"SUS4", {0, 3, 4, NONE, NONE, NONE, NONE}},
    {"SHELL", {0, 2, 6, NONE, NONE, NONE, NONE}},
}};

int32_t KeyboardLayoutChordLibrary::noteFromDegreeIndex(int32_t degreeIndex) {
	NoteSet& scaleNotes = getScaleNotes();
	int32_t scaleNoteCount = getScaleNoteCount();
	if (scaleNoteCount <= 0) {
		return getRootNote() + degreeIndex;
	}
	// Floor division, so degrees below the root land an octave down rather than folding back up
	int32_t octave = (int32_t)std::floor((float)degreeIndex / (float)scaleNoteCount);
	int32_t within = degreeIndex - octave * scaleNoteCount;
	return octave * kOctaveSize + getRootNote() + scaleNotes[within];
}

int32_t KeyboardLayoutChordLibrary::degreeIndexFromNote(int32_t note) {
	NoteSet& scaleNotes = getScaleNotes();
	int32_t scaleNoteCount = getScaleNoteCount();
	if (scaleNoteCount <= 0) {
		return note - getRootNote();
	}
	int32_t fromRoot = note - getRootNote();
	int32_t octave = (int32_t)std::floor((float)fromRoot / (float)kOctaveSize);
	int32_t within = fromRoot - octave * kOctaveSize;

	// Land on the highest scale note at or below the requested note, so toggling column modes doesn't jump
	int32_t degree = 0;
	for (int32_t idx = 0; idx < scaleNoteCount; ++idx) {
		if (scaleNotes[idx] <= within) {
			degree = idx;
		}
	}
	return octave * scaleNoteCount + degree;
}

uint8_t KeyboardLayoutChordLibrary::noteFromCoords(int32_t x) {
	KeyboardStateChordLibrary& state = getState().chordLibrary;
	if (usingScaleDegrees()) {
		return noteFromDegreeIndex(state.degreeOffset + x);
	}
	return state.noteOffset + x;
}

int32_t KeyboardLayoutChordLibrary::buildDiatonicChord(int32_t x, int32_t shapeNo,
                                                       int32_t notes[kMaxChordKeyboardSize]) {
	KeyboardStateChordLibrary& state = getState().chordLibrary;
	// Diatonic mode always counts in scale steps, whether or not the columns are showing scale degrees
	int32_t rootDegree = usingScaleDegrees() ? state.degreeOffset + x : degreeIndexFromNote(state.noteOffset + x);

	const DiatonicShape& shape = diatonicShapes[shapeNo];
	int32_t count = 0;
	for (int32_t i = 0; i < kMaxChordKeyboardSize; ++i) {
		if (shape.degrees[i] == NONE) {
			break;
		}
		notes[count++] = noteFromDegreeIndex(rootDegree + shape.degrees[i]);
	}
	return count;
}

const char* KeyboardLayoutChordLibrary::nameForIntervals(NoteSet intervals) {
	// Search both libraries, not just the active one - the jazz set names chords the default set has no
	// symbol for and vice versa, and here we only want the label
	for (const Chord& chord : defaultChordLibrary) {
		if (chord.intervalSet.toBits() == intervals.toBits() && *chord.name) {
			return chord.name;
		}
	}
	for (const Chord& chord : jazzChordLibrary) {
		if (chord.intervalSet.toBits() == intervals.toBits() && *chord.name) {
			return chord.name;
		}
	}
	return nullptr;
}

int32_t KeyboardLayoutChordLibrary::pageCount() {
	int32_t count = getState().chordLibrary.chordList.chordCount;
	return std::min<int32_t>(kVerticalPages, (count + kDisplayHeight - 1) / kDisplayHeight);
}

void KeyboardLayoutChordLibrary::evaluatePads(PressedPad presses[kMaxNumKeyboardPadPresses]) {
	currentNotesState = NotesState{}; // Erase active notes
	KeyboardStateChordLibrary& state = getState().chordLibrary;

	// We run through the presses in reverse order to display the most recent pressed chord on top
	for (int32_t idxPress = kMaxNumKeyboardPadPresses - 1; idxPress >= 0; --idxPress) {

		PressedPad pressed = presses[idxPress];

		if (pressed.x == kControlColumn) {
			// Act on the release, which is delivered exactly once. evaluatePads() replays every held pad on
			// each pad and encoder event, so a toggle driven from the press would fire over and over.
			if (!pressed.active && !pressed.dead) {
				handleControlPad(pressed.y);
			}
			continue;
		}

		if (!pressed.active || pressed.x >= kChordLibraryColumns) {
			continue;
		}

		if (usingDiatonicQuality()) {
			int32_t notes[kMaxChordKeyboardSize];
			int32_t count = buildDiatonicChord(pressed.x, pressed.y, notes);
			if (count == 0) {
				continue;
			}

			// Name the chord by matching what we built against the libraries; not every diatonic stack has a
			// symbol in them, so fall back to naming the shape instead
			NoteSet intervals;
			for (int32_t i = 0; i < count; ++i) {
				intervals.add((notes[i] - notes[0]) % kOctaveSize);
			}
			const char* name = nameForIntervals(intervals);
			drawChordName(notes[0], name ? name : "", name ? "" : diatonicShapes[pressed.y].name);

			for (int32_t i = 0; i < count; ++i) {
				enableNote(notes[i], velocity);
			}
			continue;
		}

		int32_t chordNo = getChordNo(pressed.y);

		Voicing voicing = state.chordList.getChordVoicing(chordNo);
		drawChordName(noteFromCoords(pressed.x), state.chordList.chords[chordNo].name, voicing.supplementalName);

		for (int i = 0; i < kMaxChordKeyboardSize; i++) {
			int32_t offset = voicing.offsets[i];
			if (offset == NONE) {
				continue;
			}
			enableNote(noteFromCoords(pressed.x) + offset, velocity);
		}
	}
	ColumnControlsKeyboard::evaluatePads(presses);
}

void KeyboardLayoutChordLibrary::handleControlPad(int32_t y) {
	KeyboardStateChordLibrary& state = getState().chordLibrary;

	if (y < kVerticalPages) {
		// Diatonic mode has one fixed screen of shapes, so there are no pages to jump between
		if (usingDiatonicQuality() || y >= pageCount()) {
			return;
		}
		int32_t maxRowOffset = std::max<int32_t>(0, state.chordList.chordCount - kDisplayHeight);
		state.chordList.chordRowOffset = std::min<int32_t>(y * kDisplayHeight, maxRowOffset);
	}
	else if (y == kControlRowScaleDegree || y == kControlRowDiatonic) {
		if (!scaleModesActive()) {
			char const* shortLong[2] = {"SCAL", "Needs scale mode"};
			display->displayPopup(shortLong);
			return;
		}
		if (y == kControlRowScaleDegree) {
			// Keep both column modes pointing at roughly the same note so toggling doesn't jump the keyboard
			if (state.scaleDegreeColumns) {
				state.noteOffset = noteFromDegreeIndex(state.degreeOffset);
			}
			else {
				state.degreeOffset = degreeIndexFromNote(state.noteOffset);
			}
			state.scaleDegreeColumns = !state.scaleDegreeColumns;
			char const* shortLong[2] = {"DEGR", "Scale degree columns"};
			char const* offLong[2] = {"CHRM", "Chromatic columns"};
			display->displayPopup(state.scaleDegreeColumns ? shortLong : offLong);
		}
		else {
			state.diatonicQuality = !state.diatonicQuality;
			char const* onLong[2] = {"DIAT", "Diatonic chords"};
			char const* offLong[2] = {"LIB", "Chord library"};
			display->displayPopup(state.diatonicQuality ? onLong : offLong);
		}
	}
	else if (y == kControlRowLibrary) {
		bool jazz = runtimeFeatureSettings.get(RuntimeFeatureSettingType::ChordLibrary)
		            == RuntimeFeatureStateChordLibrary::JazzChords;
		// Drive the global setting rather than adding per-clip state, so the menu and this pad stay in step.
		// It reaches the card on the next settings write, the same as a change made from the menu.
		runtimeFeatureSettings.set(RuntimeFeatureSettingType::ChordLibrary,
		                           jazz ? RuntimeFeatureStateChordLibrary::DefaultChords
		                                : RuntimeFeatureStateChordLibrary::JazzChords);
		state.chordList.refreshFromSettings();
		char const* jazzLong[2] = {"JAZZ", "Jazz library"};
		char const* defaultLong[2] = {"DFLT", "Default library"};
		display->displayPopup(jazz ? defaultLong : jazzLong);
	}

	precalculate();
	keyboardScreen.requestRendering();
}

void KeyboardLayoutChordLibrary::handleVerticalEncoder(int32_t offset) {
	if (verticalEncoderHandledByColumns(offset)) {
		return;
	}
	KeyboardStateChordLibrary& state = getState().chordLibrary;

	// Diatonic mode shows one fixed screen of shapes, so there is nothing to scroll through
	if (usingDiatonicQuality()) {
		return;
	}

	state.chordList.adjustChordRowOffset(offset);
	precalculate();
}

void KeyboardLayoutChordLibrary::handleHorizontalEncoder(int32_t offset, bool shiftEnabled,
                                                         PressedPad presses[kMaxNumKeyboardPadPresses],
                                                         bool encoderPressed) {
	if (horizontalEncoderHandledByColumns(offset, shiftEnabled)) {
		return;
	}
	KeyboardStateChordLibrary& state = getState().chordLibrary;

	if (encoderPressed) {
		for (int32_t idxPress = kMaxNumKeyboardPadPresses - 1; idxPress >= 0; --idxPress) {

			PressedPad pressed = presses[idxPress];
			if (pressed.active && pressed.x < kDisplayWidth) {

				int32_t chordNo = getChordNo(pressed.y);

				state.chordList.adjustVoicingOffset(chordNo, offset);
			}
		}
	}
	else if (usingScaleDegrees()) {
		state.degreeOffset += offset;
	}
	else {
		state.noteOffset += offset;
	}
	precalculate();
}

void KeyboardLayoutChordLibrary::precalculate() {
	KeyboardStateChordLibrary& state = getState().chordLibrary;

	// The chord set is a community feature setting, which can be changed while a clip already holds a ChordList.
	// This is a no-op unless the selection actually changed.
	state.chordList.refreshFromSettings();

	// On first render, offset by the root note. This can't be done in the constructor
	// because at constructor time, root note changes from the default menu aren't seen yet
	// or if the root note is changed in the song also isn't seen.
	if (!initializedNoteOffset) {
		initializedNoteOffset = true;
		state.noteOffset += getRootNote();
	}

	uint8_t hueStepSize = 192 / (kVerticalPages - 1); // 192 is the hue range for the rainbow
	for (int32_t i = 0; i < pageColours.size(); ++i) {
		pageColours[i] = getNoteColour(i * hueStepSize);
	}
}

void KeyboardLayoutChordLibrary::renderPads(RGB image[][kDisplayWidth + kSideBarWidth]) {
	KeyboardStateChordLibrary& state = getState().chordLibrary;
	bool inScaleMode = getScaleModeEnabled();

	// Precreate list of all scale notes per octave
	NoteSet octaveScaleNotes;
	if (inScaleMode) {
		NoteSet& scaleNotes = getScaleNotes();
		for (uint8_t idx = 0; idx < getScaleNoteCount(); ++idx) {
			octaveScaleNotes.add(scaleNotes[idx]);
		}
	}

	bool diatonic = usingDiatonicQuality();

	// Iterate over grid image
	for (int32_t x = 0; x < kChordLibraryColumns; x++) {
		int32_t noteCode = noteFromCoords(x);
		uint16_t noteWithinOctave = (uint16_t)((noteCode + kOctaveSize) - getRootNote()) % kOctaveSize;
		RGB columnColour = getNoteColour((noteCode % state.rowInterval) * state.rowColorMultiplier);

		for (int32_t y = 0; y < kDisplayHeight; ++y) {
			int32_t chordNo = getChordNo(y);

			// Diatonic shapes are built out of the scale, so every pad is playable and only the root
			// note needs picking out
			if (diatonic) {
				image[y][x] = (noteWithinOctave == 0) ? columnColour : columnColour.dim(1);
			}
			else if (inScaleMode) {
				NoteSet intervalSet = state.chordList.chords[chordNo].intervalSet;
				NoteSet modulatedNoteSet = intervalSet.modulateByOffset(noteWithinOctave);

				if (modulatedNoteSet.isSubsetOf(octaveScaleNotes)) {
					image[y][x] = columnColour;
				}
				else {
					image[y][x] = columnColour.dim(4);
				}
			}
			// Outside scale mode nothing can be checked against a scale, so just highlight the root note
			else if (noteWithinOctave == 0) {
				image[y][x] = columnColour;
			}
			else {
				image[y][x] = columnColour.dim(4);
			}
		}
	}

	renderControlColumn(image);
}

void KeyboardLayoutChordLibrary::renderControlColumn(RGB image[][kDisplayWidth + kSideBarWidth]) {
	KeyboardStateChordLibrary& state = getState().chordLibrary;

	bool diatonic = usingDiatonicQuality();
	int32_t pages = pageCount();
	int32_t currentPage = state.chordList.chordRowOffset / kDisplayHeight;

	// Page jump pads run bottom-up, matching the chord rows themselves - chord 0 sits on the bottom row
	for (int32_t y = 0; y < kVerticalPages; ++y) {
		if (diatonic || y >= pages) {
			image[y][kControlColumn] = colours::black;
		}
		else {
			image[y][kControlColumn] = (y == currentPage) ? pageColours[y] : pageColours[y].forTail();
		}
	}

	// The scale-aware modes have nothing to work from without a scale, so show them as inert
	bool scaleAvailable = scaleModesActive();
	auto toggleColour = [scaleAvailable](RGB colour, bool on) {
		if (!scaleAvailable) {
			return colours::grey;
		}
		return on ? colour : colour.forTail();
	};

	image[kControlRowScaleDegree][kControlColumn] = toggleColour(colours::darkblue, state.scaleDegreeColumns);
	image[kControlRowDiatonic][kControlColumn] = toggleColour(colours::green, state.diatonicQuality);

	bool jazz = runtimeFeatureSettings.get(RuntimeFeatureSettingType::ChordLibrary)
	            == RuntimeFeatureStateChordLibrary::JazzChords;
	image[kControlRowLibrary][kControlColumn] = jazz ? colours::orange : colours::orange.forTail();
}

void KeyboardLayoutChordLibrary::drawChordName(int16_t noteCode, const char* chordName, const char* voicingName) {
	char noteName[3] = {0};
	int32_t isNatural = 1; // gets modified inside noteCodeToString to be 0 if sharp.
	noteCodeToString(noteCode, noteName, &isNatural, false);

	char fullChordName[300];

	if (voicingName && *voicingName) {
		sprintf(fullChordName, "%s%s - %s", noteName, chordName, voicingName);
	}
	else {
		sprintf(fullChordName, "%s%s", noteName, chordName);
	}

	if (display->haveOLED()) {
		display->popupTextTemporary(fullChordName);
	}
	else {
		int8_t drawDot = !isNatural ? 0 : 255;
		display->setScrollingText(fullChordName, 0);
	}
}

bool KeyboardLayoutChordLibrary::allowSidebarType(ColumnControlFunction sidebarType) {
	if ((sidebarType == ColumnControlFunction::CHORD)) {
		return false;
	}
	return true;
}

} // namespace deluge::gui::ui::keyboard::layout
