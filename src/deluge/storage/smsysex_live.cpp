/*
 * Copyright © 2026 Synthstrom Audible Limited
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

#include "storage/smsysex_live.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/root_ui.h"
#include "gui/ui/ui.h"
#include "gui/views/automation_view.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/performance_view.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "io/midi/midi_device.h"
#include "io/midi/midi_engine.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/drum.h"
#include "model/instrument/instrument.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param.h"
#include "modulation/params/param_collection.h"
#include "modulation/params/param_descriptor.h"
#include "modulation/params/param_manager.h"
#include "modulation/patch/patch_cable_set.h"
#include "processing/engines/audio_engine.h"
#include "storage/smsysex.h"
#include "storage/storage_manager.h"
#include "util/d_string.h"
#include "util/functions.h"
#include <cstring>

// The one reply buffer, owned by smsysex.cpp. Replies are built and sent inside a single handler call,
// never held across one, so sharing it is safe.
extern JsonSerializer jWriter;

namespace {

uint32_t liveGeneration = 1;

// ---- the subscription: one cable, leased for a while, the way the OLED push (hid/hid_sysex.cpp) is
MIDICable* liveCable = nullptr;
uint32_t liveUntil = 0;

/// Set while one of our own ops changes a parameter, so the change is not pushed back as the device's.
bool suppressHooks = false;

struct PendingChange {
	ModControllable* modControllable;
	deluge::modulation::params::Kind kind;
	int32_t paramId;
	int32_t value;
};
constexpr int32_t kMaxPending = 48;
/// Per push frame: ~60 bytes an entry keeps a frame of ten under the 740-byte cap a host can carry.
constexpr int32_t kChangesPerFrame = 10;
PendingChange pending[kMaxPending];
int32_t numPending = 0;
bool pendingOverflowed = false;
uint32_t lastChangePushAt = 0;
constexpr uint32_t kChangePushInterval = kSampleRate / 40; // 25 ms, View::notifyParamAutomationOccurred's

bool dirtyPending = false;
uint32_t dirtyMarkedAt = 0;
constexpr uint32_t kDirtySettle = kSampleRate * 3 / 10; // 300 ms of quiet before "pull the preset"

/// What ^inst reports, remembered so a change can be pushed without hooking every site that makes one.
struct InstrumentSnapshot {
	Output* output = nullptr;
	Drum* drum = nullptr;
	bool entire = false;
	bool edited = false;
	bool operator==(InstrumentSnapshot const& other) const {
		return output == other.output && drum == other.drum && entire == other.entire && edited == other.edited;
	}
};
InstrumentSnapshot lastSnapshot;

bool subscribed() {
	return liveCable != nullptr && (int32_t)(liveUntil - AudioEngine::audioSampleTimer) > 0;
}

/// A name or folder as the wire can carry it: SysEx data bytes are 7-bit, and the JSON writer does not
/// escape. FAT names cannot contain a quote or backslash, so this only ever rewrites non-ASCII bytes.
void copyForWire(char const* src, char* dest, size_t maxChars) {
	size_t i = 0;
	for (; src != nullptr && src[i] != 0 && i < maxChars - 1; i++) {
		unsigned char c = src[i];
		dest[i] = (c < 0x20 || c > 0x7E || c == '"' || c == '\\') ? '?' : (char)c;
	}
	dest[i] = 0;
}

char const* outputTypeName(OutputType type) {
	switch (type) {
	case OutputType::SYNTH:
		return "synth";
	case OutputType::KIT:
		return "kit";
	case OutputType::MIDI_OUT:
		return "midi";
	case OutputType::CV:
		return "cv";
	case OutputType::AUDIO:
		return "audio";
	case OutputType::NONE:
		return "none";
	}
	return "none";
}

char const* drumTypeName(DrumType type) {
	switch (type) {
	case DrumType::SOUND:
		return "sound";
	case DrumType::MIDI:
		return "midi";
	case DrumType::GATE:
		return "gate";
	}
	return "none";
}

/// The current clip's synth or kit, or nullptr when there is no song, no clip, or the clip's output is
/// something the file protocol has no preset for.
Instrument* currentSynthOrKit() {
	if (currentSong == nullptr) {
		return nullptr;
	}
	Clip* clip = currentSong->getCurrentClip();
	if (clip == nullptr || clip->output == nullptr) {
		return nullptr;
	}
	if (clip->output->type != OutputType::SYNTH && clip->output->type != OutputType::KIT) {
		return nullptr;
	}
	return static_cast<Instrument*>(clip->output);
}

/// Consume an op's argument object when the op takes none.
void skipArguments(JsonDeserializer& reader) {
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		reader.exitTag();
	}
	reader.match('}');
}

/// The fields every ^inst-shaped reply carries.
void writeInstrumentFields() {
	Clip* clip = (currentSong != nullptr) ? currentSong->getCurrentClip() : nullptr;
	Output* output = (clip != nullptr) ? clip->output : nullptr;
	char buf[128];

	jWriter.writeAttribute("type", outputTypeName(output != nullptr ? output->type : OutputType::NONE));
	Instrument* instrument = currentSynthOrKit();
	if (instrument != nullptr) {
		copyForWire(instrument->name.get(), buf, sizeof(buf));
		jWriter.writeAttribute("name", buf);
		copyForWire(instrument->dirPath.get(), buf, sizeof(buf));
		jWriter.writeAttribute("dir", buf);
		jWriter.writeAttribute("edited", instrument->editedByUser ? 1 : 0);
		if (instrument->type == OutputType::KIT) {
			Kit* kit = static_cast<Kit*>(instrument);
			Drum* drum = kit->selectedDrum;
			jWriter.writeAttribute("drum", drum != nullptr ? kit->getDrumIndex(drum) : -1);
			jWriter.writeAttribute("drumKind", drum != nullptr ? drumTypeName(drum->type) : "none");
		}
	}
	if (clip != nullptr && clip->type == ClipType::INSTRUMENT) {
		jWriter.writeAttribute("entire", static_cast<InstrumentClip*>(clip)->affectEntire ? 1 : 0);
	}
	jWriter.writeAttribute("gen", (int32_t)liveGeneration);
}

/// Every op replies with the same tail: "err" is 0 on success, else the firmware's Error value (or 1 when
/// the failure is the protocol's own), and "why" names the failure in a word the client can act on.
void replyStatus(MIDICable& cable, JsonDeserializer& reader, char const* replyTag, Error error, char const* why,
                 bool withInstrument) {
	smSysex::startReply(jWriter, reader);
	jWriter.writeOpeningTag(replyTag, false, true);
	if (withInstrument) {
		writeInstrumentFields();
	}
	jWriter.writeAttribute("err", why != nullptr ? (error != Error::NONE ? (int32_t)error : 1) : 0);
	if (why != nullptr) {
		jWriter.writeAttribute("why", why);
	}
	jWriter.closeTag(true);
	smSysex::sendMsg(cable, jWriter);
}

/// Split "/SYNTHS/Sub/Foo.XML" into dir "SYNTHS/Sub" (no leading slash, as Instrument::dirPath holds it)
/// and name "Foo". False when the path has no folder or no .XML suffix.
bool splitPresetPath(char const* path, String* name, String* dirPath) {
	if (path == nullptr) {
		return false;
	}
	char const* lastSlash = strrchr(path, '/');
	if (lastSlash == nullptr || lastSlash == path) {
		return false;
	}
	int32_t nameLen = strlen(lastSlash + 1);
	if (nameLen <= 4 || strcasecmp(lastSlash + 1 + nameLen - 4, ".XML") != 0) {
		return false;
	}
	if (name->set(lastSlash + 1, nameLen - 4) != Error::NONE) {
		return false;
	}
	char const* dirStart = (path[0] == '/') ? path + 1 : path;
	return dirPath->set(dirStart, lastSlash - dirStart) == Error::NONE;
}

/// "/" + dir + "/" + name + ".XML"
Error joinPresetPath(String* path, String* dirPath, String* name) {
	Error error = path->set("/");
	if (error == Error::NONE) {
		error = path->concatenate(dirPath);
	}
	if (error == Error::NONE) {
		error = path->concatenate("/");
	}
	if (error == Error::NONE) {
		error = path->concatenate(name);
	}
	if (error == Error::NONE) {
		error = path->concatenate(".XML");
	}
	return error;
}

} // namespace

bool smSysex::live::enabled() {
	return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::SysexLiveEdit);
}

uint32_t smSysex::live::generation() {
	return liveGeneration;
}

void smSysex::live::bumpGeneration() {
	liveGeneration++;
}

void smSysex::live::getInstrument(MIDICable& cable, JsonDeserializer& reader) {
	skipArguments(reader);
	replyStatus(cable, reader, "^inst", Error::NONE, nullptr, true);
}

// The UI-free middle of SaveInstrumentPresetUI::performSave (gui/ui/save/save_instrument_preset_ui.cpp).
void smSysex::live::savePreset(MIDICable& cable, JsonDeserializer& reader) {
	String path;
	bool overwrite = false;
	bool keep = false;
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(&path);
		}
		else if (!strcmp(tagName, "overwrite")) {
			overwrite = reader.readTagOrAttributeValueInt() != 0;
		}
		else if (!strcmp(tagName, "keep")) {
			keep = reader.readTagOrAttributeValueInt() != 0;
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	Instrument* instrument = currentSynthOrKit();
	if (instrument == nullptr) {
		replyStatus(cable, reader, "^save", Error::NONE, "noInst", true);
		return;
	}

	// Where it goes: the path given, else the instrument's own slot, else the type's default folder.
	String name;
	String dirPath;
	if (!path.isEmpty()) {
		if (!splitPresetPath(path.get(), &name, &dirPath)) {
			replyStatus(cable, reader, "^save", Error::NONE, "path", false);
			return;
		}
	}
	else {
		name.set(&instrument->name);
		dirPath.set(&instrument->dirPath);
		if (dirPath.isEmpty()) {
			dirPath.set(getInstrumentFolder(instrument->type));
		}
		if (name.isEmpty()) {
			replyStatus(cable, reader, "^save", Error::NONE, "path", false);
			return;
		}
		Error error = joinPresetPath(&path, &dirPath, &name);
		if (error != Error::NONE) {
			replyStatus(cable, reader, "^save", error, "path", false);
			return;
		}
	}

	// Saving into a different slot: the same rules the save UI applies. A slot another instrument in
	// the song already occupies is refused; a hibernating instrument lurking in it is dropped.
	if (!keep) {
		bool differentSlot =
		    !name.equalsCaseIrrespective(&instrument->name) || !dirPath.equalsCaseIrrespective(&instrument->dirPath);
		if (differentSlot) {
			if (currentSong->getInstrumentFromPresetSlot(instrument->type, 0, 0, name.get(), dirPath.get(), false)) {
				replyStatus(cable, reader, "^save", Error::PRESET_IN_USE, "sameName", false);
				return;
			}
			currentSong->deleteHibernatingInstrumentWithSlot(instrument->type, name.get());
		}
	}

	Error error = StorageManager::createXMLFile(path.get(), smSerializer, overwrite, false);
	if (error == Error::FILE_ALREADY_EXISTS) {
		replyStatus(cable, reader, "^save", error, "exists", false);
		return;
	}
	if (error != Error::NONE) {
		replyStatus(cable, reader, "^save", error, "save", false);
		return;
	}

	instrument->writeToFile(currentSong->getCurrentClip(), currentSong);

	char const* endString = (instrument->type == OutputType::KIT) ? "\n</kit>\n" : "\n</sound>\n";
	error =
	    GetSerializer().closeFileAfterWriting(path.get(), "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", endString);
	if (error != Error::NONE) {
		replyStatus(cable, reader, "^save", error, "save", false);
		return;
	}

	if (!keep) {
		// Give the instrument in memory its new slot, exactly as the save UI does, and it is no longer
		// "edited" relative to a file: this file is it.
		instrument->name.set(&name);
		instrument->dirPath.set(&dirPath);
		instrument->mightExistOnCard = true;
		instrument->editedByUser = false;
		display->consoleText(deluge::l10n::get(deluge::l10n::String::STRING_FOR_PRESET_SAVED));
		bumpGeneration();
	}

	smSysex::startReply(jWriter, reader);
	jWriter.writeOpeningTag("^save", false, true);
	{
		char buf[256];
		copyForWire(path.get(), buf, sizeof(buf));
		jWriter.writeAttribute("path", buf);
	}
	writeInstrumentFields();
	jWriter.writeAttribute("err", (int32_t)0);
	jWriter.closeTag(true);
	smSysex::sendMsg(cable, jWriter);
}

Error smSysex::live::swapInstrumentFromFile(Instrument* oldInstrument, char const* path, String* name, String* dirPath,
                                            Instrument** newInstrumentOut) {
	*newInstrumentOut = nullptr;

	// Drop any hibernating (cached but unused) copy of the target slot, so the load reads the file and
	// the new instrument does not end up shadowed by a stale one.
	for (Instrument** prevPointer = &currentSong->firstHibernatingInstrument; *prevPointer;) {
		Instrument* instrument = *prevPointer;
		if ((instrument->type == OutputType::SYNTH || instrument->type == OutputType::KIT)
		    && !strcasecmp(instrument->name.get(), name->get())
		    && !strcasecmp(instrument->dirPath.get(), dirPath->get())) {
			*prevPointer = (Instrument*)instrument->next;
			currentSong->deleteOutput(instrument);
		}
		else {
			prevPointer = (Instrument**)&instrument->next;
		}
	}

	FilePointer filePointer;
	if (!StorageManager::fileExists(path, &filePointer)) {
		return Error::FILE_NOT_FOUND;
	}

	Instrument* newInstrument = nullptr;
	Error error = StorageManager::loadInstrumentFromFile(currentSong, nullptr, oldInstrument->type, false,
	                                                     &newInstrument, &filePointer, name, dirPath);
	if (error != Error::NONE || newInstrument == nullptr) {
		return (error != Error::NONE) ? error : Error::FILE_UNREADABLE;
	}

	error = newInstrument->loadAllAudioFiles(true);
	if (error != Error::NONE) {
		currentSong->deleteOutput(newInstrument);
		return error;
	}

	// Remember which kit row was selected, so the swap does not lose it.
	int32_t selectedDrumIndex = -1;
	if (oldInstrument->type == OutputType::KIT) {
		Kit* oldKit = static_cast<Kit*>(oldInstrument);
		if (oldKit->selectedDrum != nullptr) {
			selectedDrumIndex = oldKit->getDrumIndex(oldKit->selectedDrum);
		}
	}

	// The file is now the truth - the old in-RAM copy must be deleted, not hibernated, or it would
	// shadow the file next time the preset gets loaded.
	oldInstrument->editedByUser = false;
	currentSong->replaceInstrument(oldInstrument, newInstrument);
	currentSong->instrumentSwapped(newInstrument);

	Clip* currentClip = currentSong->getCurrentClip();
	if (currentClip != nullptr && currentClip->output == newInstrument) {
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithTimelineCounter* modelStack =
		    setupModelStackWithTimelineCounter(modelStackMemory, currentSong, currentClip);
		view.instrumentChanged(modelStack, newInstrument);
	}

	if (newInstrument->type == OutputType::KIT && selectedDrumIndex >= 0) {
		Kit* newKit = static_cast<Kit*>(newInstrument);
		Drum* drum = newKit->getDrumFromIndexAllowNull(selectedDrumIndex);
		if (drum != nullptr) {
			instrumentClipView.setSelectedDrum(drum, true, newKit);
		}
	}

	RootUI* rootUI = getRootUI();
	if (rootUI == &instrumentClipView || rootUI == &automationView) {
		instrumentClipView.recalculateColours();
	}
	uiNeedsRendering(rootUI);

	*newInstrumentOut = newInstrument;
	return Error::NONE;
}

void smSysex::live::loadPreset(MIDICable& cable, JsonDeserializer& reader) {
	String path;
	String asName;
	String asDir;
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(&path);
		}
		else if (!strcmp(tagName, "name")) {
			reader.readTagOrAttributeValueString(&asName);
		}
		else if (!strcmp(tagName, "dir")) {
			reader.readTagOrAttributeValueString(&asDir);
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	Instrument* oldInstrument = currentSynthOrKit();
	if (oldInstrument == nullptr) {
		replyStatus(cable, reader, "^load", Error::NONE, "noInst", true);
		return;
	}
	// A browser, menu or other UI stacked on the root view can hold pointers into the old instrument.
	if (getCurrentUI() != getRootUI()) {
		replyStatus(cable, reader, "^load", Error::NONE, "busy", false);
		return;
	}

	String name;
	String dirPath;
	if (!splitPresetPath(path.get(), &name, &dirPath)) {
		replyStatus(cable, reader, "^load", Error::NONE, "path", false);
		return;
	}
	// With "name"/"dir" the file is a stand-in for a preset the instrument already is (the editor's
	// TEMP/ push), so the new instrument is registered under that identity, not the file's.
	bool restoreIdentity = !asName.isEmpty();
	if (restoreIdentity) {
		name.set(&asName);
		dirPath.set(&asDir);
		if (dirPath.isEmpty()) {
			dirPath.set(getInstrumentFolder(oldInstrument->type));
		}
	}

	Instrument* newInstrument = nullptr;
	Error error = swapInstrumentFromFile(oldInstrument, path.get(), &name, &dirPath, &newInstrument);
	if (error != Error::NONE) {
		replyStatus(cable, reader, "^load", error, error == Error::FILE_NOT_FOUND ? "notFound" : "load", false);
		return;
	}

	if (restoreIdentity) {
		// The instrument differs from its file by whatever the editor changed: unsaved, like an edit
		// made on the device.
		newInstrument->editedByUser = true;
	}
	else {
		display->displayPopup(name.get());
	}
	bumpGeneration();
	replyStatus(cable, reader, "^load", Error::NONE, nullptr, true);
}

void smSysex::live::selectRow(MIDICable& cable, JsonDeserializer& reader) {
	int32_t drumIndex = -2; // -2: not given; -1: deselect
	int32_t entire = -1;
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "drum")) {
			drumIndex = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "entire")) {
			entire = reader.readTagOrAttributeValueInt() != 0 ? 1 : 0;
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	Instrument* instrument = currentSynthOrKit();
	if (instrument == nullptr || instrument->type != OutputType::KIT) {
		replyStatus(cable, reader, "^select", Error::NONE, "noKit", true);
		return;
	}
	Kit* kit = static_cast<Kit*>(instrument);
	InstrumentClip* clip = static_cast<InstrumentClip*>(currentSong->getCurrentClip());

	if (entire >= 0 && clip->affectEntire != (entire != 0)) {
		clip->affectEntire = (entire != 0);
		view.setActiveModControllableTimelineCounter(clip);
		view.setModLedStates();
		uiNeedsRendering(getRootUI());
	}

	if (drumIndex != -2) {
		Drum* drum = (drumIndex >= 0) ? kit->getDrumFromIndexAllowNull(drumIndex) : nullptr;
		if (drumIndex >= 0 && drum == nullptr) {
			replyStatus(cable, reader, "^select", Error::NONE, "noDrum", true);
			return;
		}
		instrumentClipView.setSelectedDrum(drum, true, kit);
	}

	replyStatus(cable, reader, "^select", Error::NONE, nullptr, true);
}

// Set (or read) one automatable parameter of the current instrument: MidiFollow::handleReceivedCC
// (io/midi/midi_follow.cpp) with the CC lookup and MIDI takeover removed, and with the target named
// explicitly instead of taken from the selection.
//
//   {"param": {"kind": "patched"|"unpatched"|"cable", "name": <paramNameForFile>, "src": <sourceToString>,
//              "drum": <kit row index>, "bus": 1, "value": <int32>}}
//
// "kind" is advisory: the name decides between patched and unpatched, as it does when a file is read.
// "src" makes it a patch cable amount. A kit row is addressed by "drum"; "bus" addresses the kit's own
// parameters (what AFFECT ENTIRE reaches); neither means the selected row. Without "value" the current
// value is reported and nothing changes.
void smSysex::live::setParam(MIDICable& cable, JsonDeserializer& reader) {
	using namespace deluge::modulation::params;

	String name;
	String source;
	int32_t drumIndex = -1;
	bool bus = false;
	int32_t value = 0;
	bool haveValue = false;
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "name")) {
			reader.readTagOrAttributeValueString(&name);
		}
		else if (!strcmp(tagName, "src")) {
			reader.readTagOrAttributeValueString(&source);
		}
		else if (!strcmp(tagName, "drum")) {
			drumIndex = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "bus")) {
			bus = reader.readTagOrAttributeValueInt() != 0;
		}
		else if (!strcmp(tagName, "value")) {
			value = reader.readTagOrAttributeValueInt();
			haveValue = true;
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	Instrument* instrument = currentSynthOrKit();
	if (instrument == nullptr) {
		replyStatus(cable, reader, "^param", Error::NONE, "noInst", true);
		return;
	}
	Clip* clip = currentSong->getCurrentClip();
	bool kitBus = (instrument->type == OutputType::KIT) && bus;

	// The name resolves the way Sound::readParamsFromFile / GlobalEffectable resolve it: patched names live
	// below UNPATCHED_START, unpatched ones above, and the kit bus has its own unpatched table.
	Kind kind;
	int32_t paramId;
	if (!source.isEmpty()) {
		ParamType destination = fileStringToParam(Kind::PATCHED, name.get(), true);
		if (destination >= UNPATCHED_START) {
			replyStatus(cable, reader, "^param", Error::NONE, "name", false);
			return;
		}
		PatchSource patchSource = stringToSource(source.get());
		if (patchSource == PatchSource::NONE) {
			replyStatus(cable, reader, "^param", Error::NONE, "src", false);
			return;
		}
		ParamDescriptor descriptor;
		descriptor.setToHaveParamOnly(destination);
		kind = Kind::PATCH_CABLE;
		paramId = PatchCableSet::getParamId(descriptor, patchSource);
	}
	else if (kitBus) {
		ParamType p = fileStringToParam(Kind::UNPATCHED_GLOBAL, name.get(), false);
		if (p >= kUnpatchedAndPatchedMaximum || p < UNPATCHED_START) {
			replyStatus(cable, reader, "^param", Error::NONE, "name", false);
			return;
		}
		kind = Kind::UNPATCHED_GLOBAL;
		paramId = p - UNPATCHED_START;
	}
	else {
		ParamType p = fileStringToParam(Kind::UNPATCHED_SOUND, name.get(), true);
		if (p >= kUnpatchedAndPatchedMaximum) {
			replyStatus(cable, reader, "^param", Error::NONE, "name", false);
			return;
		}
		if (p < UNPATCHED_START) {
			kind = Kind::PATCHED;
			paramId = p;
		}
		else {
			kind = Kind::UNPATCHED_SOUND;
			paramId = p - UNPATCHED_START;
		}
	}

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	ModelStackWithAutoParam* modelStackWithParam = nullptr;

	if (instrument->type == OutputType::SYNTH || kitBus) {
		modelStackWithParam = instrument->getModelStackWithParam(modelStack, clip, paramId, kind, kitBus, false);
	}
	else {
		// A kit row, by index rather than by selection. Its ParamManager lives in the clip's note row for
		// that drum, or in the song's backup when the drum has no row in this clip.
		Kit* kit = static_cast<Kit*>(instrument);
		Drum* drum = (drumIndex >= 0) ? kit->getDrumFromIndexAllowNull(drumIndex) : kit->selectedDrum;
		if (drum == nullptr || drum->type != DrumType::SOUND) {
			replyStatus(cable, reader, "^param", Error::NONE, "noDrum", false);
			return;
		}
		InstrumentClip* instrumentClip = static_cast<InstrumentClip*>(clip);
		ModelStackWithNoteRow* modelStackWithNoteRow = instrumentClip->getNoteRowForDrum(modelStack, drum);
		ModelStackWithThreeMainThings* modelStackWithThreeMainThings = nullptr;
		if (modelStackWithNoteRow->getNoteRowAllowNull() != nullptr) {
			modelStackWithThreeMainThings = modelStackWithNoteRow->addOtherTwoThingsAutomaticallyGivenNoteRow();
		}
		else {
			ModControllableAudio* modControllable = static_cast<ModControllableAudio*>(drum->toModControllable());
			ParamManager* paramManager = currentSong->getBackedUpParamManagerPreferablyWithClip(modControllable, clip);
			if (paramManager == nullptr) {
				replyStatus(cable, reader, "^param", Error::NONE, "noRow", false);
				return;
			}
			modelStackWithThreeMainThings = modelStack->addOtherTwoThingsButNoNoteRow(modControllable, paramManager);
		}
		if (modelStackWithThreeMainThings != nullptr) {
			if (kind == Kind::PATCHED) {
				modelStackWithParam = modelStackWithThreeMainThings->getPatchedAutoParamFromId(paramId);
			}
			else if (kind == Kind::UNPATCHED_SOUND) {
				modelStackWithParam = modelStackWithThreeMainThings->getUnpatchedAutoParamFromId(paramId);
			}
			else {
				modelStackWithParam = modelStackWithThreeMainThings->getPatchCableAutoParamFromId(paramId);
			}
		}
	}

	if (modelStackWithParam == nullptr || modelStackWithParam->autoParam == nullptr) {
		replyStatus(cable, reader, "^param", Error::NONE, "noParam", false);
		return;
	}

	if (haveValue) {
		// Step editing (a held note region in the view) is honoured the way MIDI Follow honours it.
		int32_t modPos = 0;
		int32_t modLength = 0;
		if (view.modLength && clip == view.activeModControllableModelStack.getTimelineCounterAllowNull()) {
			modPos = view.modPos;
			modLength = view.modLength;
		}
		suppressHooks = true;
		modelStackWithParam->autoParam->setValuePossiblyForRegion(value, modelStackWithParam, modPos, modLength);
		instrument->beenEdited(false);
		suppressHooks = false;
		bumpGeneration();

		int32_t knobPos = modelStackWithParam->paramCollection->paramValueToKnobPos(
		    modelStackWithParam->autoParam->getCurrentValue(), modelStackWithParam);
		bool editingInView = false;
		RootUI* rootUI = getRootUI();
		if (rootUI == &automationView) {
			editingInView =
			    automationView.possiblyRefreshAutomationEditorGrid(clip, kind, modelStackWithParam->paramId);
		}
		else if (rootUI == &performanceView) {
			editingInView =
			    performanceView.possiblyRefreshPerformanceViewDisplay(kind, modelStackWithParam->paramId, knobPos);
		}
		if (midiEngine.midiFollowDisplayParam && !editingInView) {
			view.displayModEncoderValuePopup(kind, modelStackWithParam->paramId, knobPos);
		}
	}

	smSysex::startReply(jWriter, reader);
	jWriter.writeOpeningTag("^param", false, true);
	{
		char buf[64];
		copyForWire(name.get(), buf, sizeof(buf));
		jWriter.writeAttribute("name", buf);
		if (!source.isEmpty()) {
			copyForWire(source.get(), buf, sizeof(buf));
			jWriter.writeAttribute("src", buf);
		}
	}
	if (instrument->type == OutputType::KIT) {
		if (kitBus) {
			jWriter.writeAttribute("bus", 1);
		}
		else {
			jWriter.writeAttribute("drum", drumIndex);
		}
	}
	jWriter.writeAttribute("value", modelStackWithParam->autoParam->getCurrentValue());
	jWriter.writeAttribute("err", (int32_t)0);
	jWriter.closeTag(true);
	smSysex::sendMsg(cable, jWriter);
}

void smSysex::live::subscribe(MIDICable& cable, JsonDeserializer& reader) {
	int32_t secs = 10;
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "secs")) {
			secs = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	if (secs <= 0) {
		if (liveCable == &cable) {
			liveCable = nullptr;
		}
	}
	else {
		if (secs > 120) {
			secs = 120;
		}
		bool fresh = (liveCable != &cable) || !subscribed();
		liveCable = &cable;
		liveUntil = AudioEngine::audioSampleTimer + (uint32_t)secs * kSampleRate;
		if (fresh) {
			numPending = 0;
			pendingOverflowed = false;
			dirtyPending = false;
		}
	}

	// The reply carries the instrument, and that is the baseline the pushes are measured from.
	InstrumentSnapshot snapshot;
	{
		Clip* clip = (currentSong != nullptr) ? currentSong->getCurrentClip() : nullptr;
		snapshot.output = (clip != nullptr) ? clip->output : nullptr;
		Instrument* instrument = currentSynthOrKit();
		snapshot.drum = (instrument != nullptr && instrument->type == OutputType::KIT)
		                    ? static_cast<Kit*>(instrument)->selectedDrum
		                    : nullptr;
		snapshot.entire =
		    (clip != nullptr && clip->type == ClipType::INSTRUMENT) && static_cast<InstrumentClip*>(clip)->affectEntire;
		snapshot.edited = (instrument != nullptr) && instrument->editedByUser;
	}
	lastSnapshot = snapshot;

	smSysex::startReply(jWriter, reader);
	jWriter.writeOpeningTag("^sub", false, true);
	jWriter.writeAttribute("secs", secs > 0 ? secs : 0);
	writeInstrumentFields();
	jWriter.writeAttribute("err", (int32_t)0);
	jWriter.closeTag(true);
	smSysex::sendMsg(cable, jWriter);
}

void smSysex::live::noteParamChanged(ModelStackWithAutoParam const* modelStack) {
	using namespace deluge::modulation::params;
	liveGeneration++;
	if (suppressHooks || !subscribed() || modelStack == nullptr || modelStack->autoParam == nullptr
	    || modelStack->paramCollection == nullptr) {
		return;
	}
	Kind kind = modelStack->paramCollection->getParamKind();
	if (kind != Kind::PATCHED && kind != Kind::UNPATCHED_SOUND && kind != Kind::UNPATCHED_GLOBAL
	    && kind != Kind::PATCH_CABLE) {
		return; // MPE expression and MIDI CC "params" are not in a preset
	}
	ModControllable* modControllable = modelStack->modControllable;
	int32_t paramId = modelStack->paramId;
	int32_t value = modelStack->autoParam->getCurrentValue();
	for (int32_t i = 0; i < numPending; i++) {
		if (pending[i].modControllable == modControllable && pending[i].kind == kind && pending[i].paramId == paramId) {
			pending[i].value = value;
			return;
		}
	}
	if (numPending >= kMaxPending) {
		pendingOverflowed = true; // the drain turns this into a ^dirty, and the client pulls
		return;
	}
	pending[numPending++] = PendingChange{modControllable, kind, paramId, value};
}

void smSysex::live::noteEdited(Instrument* instrument) {
	liveGeneration++;
	if (suppressHooks || !subscribed()) {
		return;
	}
	if (instrument != currentSynthOrKit()) {
		return;
	}
	dirtyPending = true;
	dirtyMarkedAt = AudioEngine::audioSampleTimer;
}

namespace {

/// Where a pending change lives in the current instrument: the synth itself, the kit bus ("b"), or a
/// kit row by index ("d"). False when it belongs to something else (a previous instrument, another clip).
bool locateChange(Instrument* instrument, ModControllable* modControllable, bool* isBus, int32_t* drumIndex) {
	*isBus = false;
	*drumIndex = -1;
	if (modControllable == instrument->toModControllable()) {
		*isBus = (instrument->type == OutputType::KIT);
		return true;
	}
	if (instrument->type != OutputType::KIT) {
		return false;
	}
	int32_t index = 0;
	for (Drum* drum = static_cast<Kit*>(instrument)->firstDrum; drum != nullptr; drum = drum->next, index++) {
		if (drum->toModControllable() == modControllable) {
			*drumIndex = index;
			return true;
		}
	}
	return false;
}

/// One entry of a ^chg frame: {"n": name, "s": source (cables), "v": value, "d": row | "b": 1}.
/// False when the param has no file name (a nested cable, an unknown id) and is better left out.
bool writeChange(PendingChange const& change, bool isBus, int32_t drumIndex) {
	using namespace deluge::modulation::params;
	char const* name = nullptr;
	char const* source = nullptr;
	switch (change.kind) {
	case Kind::PATCHED:
		name = paramNameForFile(Kind::PATCHED, change.paramId);
		break;
	case Kind::UNPATCHED_SOUND:
		name = paramNameForFile(Kind::UNPATCHED_SOUND, change.paramId + UNPATCHED_START);
		break;
	case Kind::UNPATCHED_GLOBAL:
		name = paramNameForFile(Kind::UNPATCHED_GLOBAL, change.paramId + UNPATCHED_START);
		break;
	case Kind::PATCH_CABLE: {
		ParamDescriptor destination;
		PatchSource patchSource;
		PatchCableSet::dissectParamId(change.paramId, &destination, &patchSource);
		if (!destination.isJustAParam()) {
			return false; // a cable modulating another cable's depth: not addressed by this protocol
		}
		name = paramNameForFile(Kind::PATCHED, destination.getJustTheParam());
		source = sourceToString(patchSource);
		break;
	}
	default:
		return false;
	}
	if (name == nullptr || name[0] == 0) {
		return false;
	}
	jWriter.writeOpeningTag(nullptr, true);
	jWriter.writeAttribute("n", name);
	if (source != nullptr) {
		jWriter.writeAttribute("s", source);
	}
	if (isBus) {
		jWriter.writeAttribute("b", 1);
	}
	else if (drumIndex >= 0) {
		jWriter.writeAttribute("d", drumIndex);
	}
	jWriter.writeAttribute("v", change.value);
	jWriter.closeTag();
	return true;
}

/// Room in the cable's send ring for a frame, with the same margin the OLED push keeps.
bool canPush() {
	return liveCable->sendBufferSpace() >= 1024;
}

void pushInstrument() {
	smSysex::startDirect(jWriter);
	jWriter.writeOpeningTag("^inst", false, true);
	writeInstrumentFields();
	jWriter.closeTag(true);
	smSysex::sendMsg(*liveCable, jWriter);
}

void pushDirty() {
	smSysex::startDirect(jWriter);
	jWriter.writeOpeningTag("^dirty", false, true);
	jWriter.writeAttribute("gen", (int32_t)liveGeneration);
	jWriter.closeTag(true);
	smSysex::sendMsg(*liveCable, jWriter);
}

/// Send the pending changes, ten to a frame, dropping any that no longer belong to the current instrument.
void pushChanges(Instrument* instrument) {
	int32_t i = 0;
	while (i < numPending) {
		if (!canPush()) {
			// Keep what is left for the next tick.
			int32_t remaining = numPending - i;
			memmove(pending, pending + i, remaining * sizeof(PendingChange));
			numPending = remaining;
			return;
		}
		smSysex::startDirect(jWriter);
		jWriter.writeOpeningTag("^chg", false, true);
		jWriter.writeAttribute("gen", (int32_t)liveGeneration);
		jWriter.writeArrayStart("p", true, false);
		int32_t written = 0;
		for (; i < numPending && written < kChangesPerFrame; i++) {
			bool isBus;
			int32_t drumIndex;
			if (!locateChange(instrument, pending[i].modControllable, &isBus, &drumIndex)) {
				continue;
			}
			if (writeChange(pending[i], isBus, drumIndex)) {
				written++;
			}
		}
		jWriter.writeArrayEnding("p", false, false);
		jWriter.closeTag(true);
		if (written > 0) {
			smSysex::sendMsg(*liveCable, jWriter);
		}
	}
	numPending = 0;
}

} // namespace

void smSysex::live::tick() {
	if (liveCable == nullptr) {
		return;
	}
	if (!subscribed()) {
		// The lease ran out: forget the client and whatever was queued for it.
		liveCable = nullptr;
		numPending = 0;
		pendingOverflowed = false;
		dirtyPending = false;
		return;
	}
	uint32_t now = AudioEngine::audioSampleTimer;

	// The instrument, row, AFFECT ENTIRE or edited flag changed: say so first, and drop changes that were
	// queued against whatever was current before.
	InstrumentSnapshot snapshot;
	Clip* clip = (currentSong != nullptr) ? currentSong->getCurrentClip() : nullptr;
	snapshot.output = (clip != nullptr) ? clip->output : nullptr;
	Instrument* instrument = currentSynthOrKit();
	snapshot.drum = (instrument != nullptr && instrument->type == OutputType::KIT)
	                    ? static_cast<Kit*>(instrument)->selectedDrum
	                    : nullptr;
	snapshot.entire =
	    (clip != nullptr && clip->type == ClipType::INSTRUMENT) && static_cast<InstrumentClip*>(clip)->affectEntire;
	snapshot.edited = (instrument != nullptr) && instrument->editedByUser;
	if (!(snapshot == lastSnapshot)) {
		if (!canPush()) {
			return;
		}
		if (snapshot.output != lastSnapshot.output) {
			numPending = 0;
			pendingOverflowed = false;
			dirtyPending = false;
		}
		pushInstrument();
		lastSnapshot = snapshot;
	}
	if (instrument == nullptr) {
		numPending = 0;
		pendingOverflowed = false;
		dirtyPending = false;
		return;
	}

	if (pendingOverflowed) {
		// Too much moved at once to list it: the client pulls the preset instead.
		numPending = 0;
		pendingOverflowed = false;
		dirtyPending = true;
		dirtyMarkedAt = now;
	}

	if (numPending > 0 && (uint32_t)(now - lastChangePushAt) >= kChangePushInterval && canPush()) {
		pushChanges(instrument);
		lastChangePushAt = now;
	}

	if (dirtyPending && (uint32_t)(now - dirtyMarkedAt) >= kDirtySettle && canPush()) {
		dirtyPending = false;
		pushDirty();
	}
}
