#include "storage/smsysex.h"
#include "fatfs/ff.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/root_ui.h"
#include "gui/ui/ui.h"
#include "gui/ui_timer_manager.h"
#include "gui/views/arranger_view.h"
#include "gui/views/automation_view.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/session_view.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/display/seven_segment.h"
#include "hid/hid_sysex.h"
#include "io/debug/log.h"
#include "io/debug/print.h"
#include "io/midi/midi_device.h"
#include "io/midi/midi_engine.h"
#include "io/midi/sysex.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/audio_clip.h"
#include "model/clip/clip.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/instrument.h"
#include "model/instrument/midi_instrument.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/audio_output.h"
#include "processing/engines/audio_engine.h"
#include "scheduler_api.h"
#include "util/containers.h"
#include "util/d_stringbuf.h"
#include "util/pack.h"
#include <cstring>

#define MAX_DIR_LINES 25

extern "C" {
extern uint8_t currentlyAccessingCard;
}

// Last-frame USB SysEx receive stats from midi_engine.cpp, surfaced in the ^echo reply.
extern uint32_t lastSysexRxEvents;
extern uint32_t lastSysexRxLastCIN;
extern uint32_t lastSysexRxLen;
extern uint32_t sysexRxOverflowBails;
DIR sxDIR;
uint32_t dirOffsetCounter;

JsonSerializer jWriter;
String activeDirName;
const size_t blockBufferMax = 1024;
const size_t sysexBufferMax = blockBufferMax + 256;
// A whole frame carrying a blockBufferMax data block must survive reassembly in the cable's buffer.
static_assert(sizeof(MIDICable::incomingSysexBuffer) >= sysexBufferMax,
              "incomingSysexBuffer too small for a full smSysex block");
uint8_t* writeBlockBuffer = nullptr;
uint8_t* readBlockBuffer = nullptr;
// Advertised as "pipe" in the ^session grant: max concurrent requests whose replies fit in the USB
// send ring (a full-block ^read reply spans ~410 of its 1024 events).
const uint32_t kMaxPipelinedRequests = 2;
const uint32_t MAX_OPEN_FILES = 4;

struct FILdata {
	String fName;
	uint32_t fileID;
	uint32_t LRUstamp = 0;
	uint32_t fSize = 0;
	uint32_t fPosition = 0; // file offset noted after last read/write operation.
	bool fileOpen = false;
	int forWrite = false;
	FIL file;
};

int32_t FIDcounter = 1;
uint32_t LRUcounter = 1;

PLACE_SDRAM_BSS FILdata openFiles[MAX_OPEN_FILES];

const int MaxSysExLength = 1024;

struct SysExDataEntry {
	MIDICable& cable;
	int32_t len;
	uint8_t data[sysexBufferMax];

	SysExDataEntry(MIDICable& forCable, int32_t newLen) : cable{forCable}, len{newLen} {}
};

deluge::deque<SysExDataEntry> SysExQ;

// The following constants assume that the messageID part ranges from 1 to SYSEX_MSGID_MAX
// and that SYSEX_MSGID_MAX is 1 less than a power of 2.
// It also assumes that MAX_SYSEX_SESSIONS is also 1 less than a power of 2.
const uint32_t MAX_SYSEX_SESSIONS = 15;
const uint32_t SYSEX_MSGID_MAX = 7;
const uint32_t SYSEX_MSGID_MASK = 0x07;
const uint32_t SYSEX_SESSION_MASK = 0x78;
const uint32_t SYSEX_SESSION_SHIFT = 3;

uint32_t session_mono_counter = 1;
uint32_t sessionLRU_array[MAX_SYSEX_SESSIONS + 1] = {0};

void smSysex::noteSessionIdUse(uint8_t msgId) {
	uint32_t sessionNum = (msgId & SYSEX_SESSION_MASK) >> SYSEX_SESSION_SHIFT;
	sessionLRU_array[sessionNum] = session_mono_counter++;
}

void smSysex::noteFileIdUse(FILdata* fp) {
	fp->LRUstamp = LRUcounter++;
}

// Returns the entry in the FILdata array for the given fid
FILdata* smSysex::entryForFID(uint32_t fid) {
	for (int i = 0; i < MAX_OPEN_FILES; ++i) {
		if (openFiles[i].fileID == fid) {
			return openFiles + i;
		}
	}
	return nullptr;
}

// Assign a FIL from our pool.
FILdata* smSysex::findEmptyFIL() {
	uint32_t LRUtime = 0xFFFFFFFF;
	uint32_t LRUindex = 0;

	for (int i = 0; i < MAX_OPEN_FILES; ++i) {
		if (!openFiles[i].fileOpen) {
			return openFiles + i;
		}
		else if (openFiles[i].LRUstamp < LRUtime) {
			LRUtime = openFiles[i].LRUstamp;
			LRUindex = i;
		}
	}
	// Close the abandoned file before we reuse the entry.
	FILdata* oldest = openFiles + LRUindex;
	closeFIL(oldest);
	return oldest;
}

void smSysex::startDirect(JsonSerializer& writer) {
	writer.reset();
	writer.setMemoryBased();
	uint8_t reply_hdr[7] = {0xf0, 0x00, 0x21, 0x7B, 0x01, SysEx::SysexCommands::Json, 0};
	writer.writeBlock(reply_hdr, sizeof(reply_hdr));
}

void smSysex::startReply(JsonSerializer& writer, JsonDeserializer& reader) {
	writer.reset();
	writer.setMemoryBased();
	uint8_t reply_hdr[7] = {0xf0, 0x00, 0x21, 0x7B, 0x01, SysEx::SysexCommands::JsonReply, reader.getReplySeqNum()};
	writer.writeBlock(reply_hdr, sizeof(reply_hdr));
}

void smSysex::sendMsg(MIDICable& cable, JsonSerializer& writer) {
	writer.writeByte(0xF7);

	char* bitz = writer.getBufferPtr();
	int32_t bw = writer.bytesWritten();
	cable.sendSysex((const uint8_t*)bitz, bw);
};

FILdata* smSysex::openFIL(const char* fPath, int forWrite, uint32_t* fsize, FRESULT* eCode) {
	FILdata* fp = findEmptyFIL();
	fp->fName.set(fPath);
	fp->fileID = FIDcounter++;
	noteFileIdUse(fp);
	BYTE mode = FA_READ;
	if (forWrite) {
		mode = FA_WRITE;
		if (forWrite == 1)
			mode |= FA_CREATE_ALWAYS;
	}
	FRESULT err = f_open(&fp->file, fPath, mode);
	*eCode = err;
	if (err == FRESULT::FR_OK) {
		fp->fileOpen = true;
		fp->forWrite = forWrite;
		fp->fSize = f_size(&fp->file);
		fp->fPosition = 0;
		return fp;
	}
	return nullptr;
}

FRESULT smSysex::closeFIL(FILdata* fp) {
	if (fp == nullptr)
		return FRESULT::FR_INVALID_OBJECT;

	FRESULT err = f_close(&fp->file);
	fp->fileOpen = false;
	fp->forWrite = 0;
	fp->fSize = 0;
	return err;
}

// Fill in missing directories for the full path name given.
// Unless the last character in the path is a /, we assume the
// path given ends with a filename (which we ignore).
FRESULT smSysex::createPathDirectories(String& path, uint32_t date, uint32_t time) {
	FRESULT errCode;
	if (path.getLength() > 256) {
		return FRESULT::FR_INVALID_PARAMETER;
	}

	char working[257];
	char pathPart[257];
	strcpy(working, path.get());
	// ignore the file name and extension part.
	int len = strlen(working);
	int lastSlash;
	for (lastSlash = len - 1; lastSlash >= 0; lastSlash--) {
		if (working[lastSlash] == '/')
			break;
	}
	if (lastSlash == 0)
		return FRESULT::FR_INVALID_PARAMETER;

	int jx = 1; // skip the leading slash.
	while (jx <= lastSlash) {
		if (working[jx] == '/') {
			DIR wDIR;
			working[jx] = 0;
			strcpy(pathPart, working);
			working[jx] = '/';
			if (strlen(pathPart)) {
				errCode = f_opendir(&wDIR, (TCHAR*)pathPart);
				if (errCode == FRESULT::FR_NO_PATH) {
					errCode = f_mkdir((TCHAR*)pathPart);
					if (errCode == FRESULT::FR_OK && (date != 0 || time != 0)) {
						FILINFO finfo;
						finfo.fdate = date;
						finfo.ftime = time;
						errCode = f_utime(pathPart, &finfo);
					}
				}
				else if (errCode == 0) {
					errCode = f_closedir(&wDIR);
				}
				else {
					return errCode;
				}
			}
		}
		jx++;
	}
	return errCode;
}

void smSysex::openFile(MIDICable& cable, JsonDeserializer& reader) {
	bool forWrite = false;
	String path;
	int32_t rn = 0;
	char const* tagName;
	uint32_t date = 0;
	uint32_t time = 0;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "write")) {
			forWrite = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(&path);
		}
		// Since you can't change the date/time of an open file, we use date/time
		// only for created directoris.
		else if (!strcmp(tagName, "date")) {
			date = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "time")) {
			time = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}

	reader.match('}');
	bool pathCreateTried = false;
retry:
	FRESULT errCode;
	uint32_t fSize = 0;

	FILdata* fp = openFIL(path.get(), forWrite, &fSize, &errCode);

	if (fp != nullptr) {
		fSize = fp->fSize;
	}
	if (forWrite && !pathCreateTried && errCode == FRESULT::FR_NO_PATH) { // was the path missing?
		createPathDirectories(path, date, time);
		pathCreateTried = true;
		goto retry;
	}

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^open", false, true);
	jWriter.writeAttribute("fid", fp != nullptr ? fp->fileID : 0);
	jWriter.writeAttribute("size", fSize);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);

	sendMsg(cable, jWriter);
}

// If a preset XML file just written over sysex belongs to a synth/kit the song has cached in RAM,
// discard the cached copy and reload it from the file, so edits saved from an external editor are
// heard immediately without cloning the preset or rebooting.
static void reloadPresetFromFile(const char* path) {
	if (!runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::SysexPresetReload)) {
		return;
	}
	if (currentSong == nullptr) {
		return;
	}

	const char* lastSlash = strrchr(path, '/');
	if (lastSlash == nullptr || lastSlash == path) {
		return;
	}
	int32_t nameLen = strlen(lastSlash + 1);
	if (nameLen <= 4 || strcasecmp(lastSlash + 1 + nameLen - 4, ".XML") != 0) {
		return;
	}

	String presetName;
	if (presetName.set(lastSlash + 1, nameLen - 4) != Error::NONE) {
		return;
	}
	// Instrument dirPaths are stored without a leading slash ("SYNTHS", "KITS/Sub", ...).
	const char* dirStart = (path[0] == '/') ? path + 1 : path;
	String dirPath;
	if (dirPath.set(dirStart, lastSlash - dirStart) != Error::NONE) {
		return;
	}

	// Don't swap instruments while a browser, menu or other UI is stacked on top of the root
	// view - those can hold pointers into the old Instrument.
	if (getCurrentUI() != getRootUI()) {
		return;
	}

	// Drop any hibernating (cached but unused) copy, so the next load reads the file.
	for (Instrument** prevPointer = &currentSong->firstHibernatingInstrument; *prevPointer;) {
		Instrument* instrument = *prevPointer;
		if ((instrument->type == OutputType::SYNTH || instrument->type == OutputType::KIT)
		    && !strcasecmp(instrument->name.get(), presetName.get())
		    && !strcasecmp(instrument->dirPath.get(), dirPath.get())) {
			*prevPointer = (Instrument*)instrument->next;
			currentSong->deleteOutput(instrument);
		}
		else {
			prevPointer = (Instrument**)&instrument->next;
		}
	}

	// See whether the preset is in use in the song.
	Instrument* oldInstrument = nullptr;
	for (Output* output = currentSong->firstOutput; output; output = output->next) {
		if ((output->type == OutputType::SYNTH || output->type == OutputType::KIT)
		    && !strcasecmp(output->name.get(), presetName.get())
		    && !strcasecmp(((Instrument*)output)->dirPath.get(), dirPath.get())) {
			oldInstrument = (Instrument*)output;
			break;
		}
	}
	if (oldInstrument == nullptr) {
		return;
	}

	FilePointer filePointer;
	if (!StorageManager::fileExists(path, &filePointer)) {
		return;
	}

	Instrument* newInstrument = nullptr;
	Error error = StorageManager::loadInstrumentFromFile(currentSong, nullptr, oldInstrument->type, false,
	                                                     &newInstrument, &filePointer, &presetName, &dirPath);
	if (error != Error::NONE || newInstrument == nullptr) {
		return;
	}

	error = newInstrument->loadAllAudioFiles(true);
	if (error != Error::NONE) {
		currentSong->deleteOutput(newInstrument);
		return;
	}

	// The file is now the truth - the old in-RAM copy must be deleted, not hibernated, or it would
	// shadow the file next time the preset gets loaded.
	oldInstrument->editedByUser = false;
	currentSong->replaceInstrument(oldInstrument, newInstrument);
	currentSong->instrumentSwapped(newInstrument);

	Clip* currentClip = getCurrentClip();
	if (currentClip && currentClip->output == newInstrument) {
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithTimelineCounter* modelStack =
		    setupModelStackWithTimelineCounter(modelStackMemory, currentSong, currentClip);
		view.instrumentChanged(modelStack, newInstrument);
	}

	RootUI* rootUI = getRootUI();
	if (rootUI == &instrumentClipView || rootUI == &automationView) {
		instrumentClipView.recalculateColours();
	}
	uiNeedsRendering(rootUI);

	display->displayPopup(presetName.get());
}

void smSysex::closeFile(MIDICable& cable, JsonDeserializer& reader) {
	int32_t fid = 0;
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "fid")) {
			fid = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	FILdata* fd = entryForFID(fid);
	bool wasWrite = fd != nullptr && fd->fileOpen && fd->forWrite;
	String writtenPath;
	if (wasWrite) {
		writtenPath.set(&fd->fName);
	}
	FRESULT errCode = closeFIL(fd);

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^close", false, true);
	jWriter.writeAttribute("fid", (uint32_t)fid);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);

	sendMsg(cable, jWriter);

	// Reply first so the sender isn't stalled, then reload the preset if the song has it loaded.
	if (wasWrite && errCode == FRESULT::FR_OK) {
		reloadPresetFromFile(writtenPath.get());
	}
}

void smSysex::deleteFile(MIDICable& cable, JsonDeserializer& reader) {
	FRESULT errCode = FRESULT::FR_OK;

	char const* tagName;
	String path;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(&path);
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	const char* pathVal = path.get();
	const TCHAR* pathTC = (const TCHAR*)pathVal;

	if (pathTC && strlen(pathTC) > 0) {
		D_PRINTLN(pathTC);
		errCode = f_unlink(pathTC);
		startReply(jWriter, reader);
		jWriter.writeOpeningTag("^delete", false, true);
		jWriter.writeAttribute("err", errCode);
		jWriter.closeTag(true);
		sendMsg(cable, jWriter);
	}
}

void smSysex::createDirectory(MIDICable& cable, JsonDeserializer& reader) {
	FRESULT errCode = FRESULT::FR_OK;

	char const* tagName;
	String path;
	uint32_t date = 0;
	uint32_t time = 0;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(&path);
		}
		else if (!strcmp(tagName, "date")) {
			date = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "time")) {
			time = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	const char* pathVal = path.get();
	const TCHAR* pathTC = (const TCHAR*)pathVal;

	if (pathTC && strlen(pathTC) > 0) {
		D_PRINTLN(pathTC);
		errCode = f_mkdir(pathTC);
		if (errCode == FRESULT::FR_OK && (date != 0 || time != 0)) {
			FILINFO finfo;
			finfo.fdate = date;
			finfo.ftime = time;
			errCode = f_utime(pathTC, &finfo);
		}
		startReply(jWriter, reader);
		jWriter.writeOpeningTag("^mkdir", false, true);
		jWriter.writeAttribute("path", pathVal);
		jWriter.writeAttribute("err", errCode);
		jWriter.closeTag(true);
		sendMsg(cable, jWriter);
	}
}

void smSysex::rename(MIDICable& cable, JsonDeserializer& reader) {
	FRESULT errCode = FRESULT::FR_OK;

	char const* tagName;
	String fromName;
	String toName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "from")) {
			reader.readTagOrAttributeValueString(&fromName);
		}
		else if (!strcmp(tagName, "to")) {
			reader.readTagOrAttributeValueString(&toName);
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	const char* fromVal = fromName.get();
	const TCHAR* fromTC = (const TCHAR*)fromVal;
	const char* toVal = toName.get();
	const TCHAR* toTC = (const TCHAR*)toVal;

	if (fromTC && strlen(fromTC) && toTC && strlen(toTC)) {
		D_PRINTLN(fromTC);
		D_PRINTLN(toTC);
		errCode = f_rename(fromTC, toTC);
		startReply(jWriter, reader);
		jWriter.writeOpeningTag("^rename", false, true);
		jWriter.writeAttribute("from", fromVal);
		jWriter.writeAttribute("to", toVal);
		jWriter.writeAttribute("err", errCode);
		jWriter.closeTag(true);
		sendMsg(cable, jWriter);
	}
}

// Returns a block of directory entries as a Json array.
void smSysex::getDirEntries(MIDICable& cable, JsonDeserializer& reader) {
	String path;
	path.set("/");
	uint32_t lineOffset = 0;
	uint32_t linesWanted = 20;

	FRESULT errCode = FRESULT::FR_OK;
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "offset")) {
			lineOffset = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "lines")) {
			linesWanted = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(&path);
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');
	if (linesWanted > MAX_DIR_LINES)
		linesWanted = MAX_DIR_LINES;
	// We should pick up on path changes and out-of-order offset requests.

	const char* pathVal = path.get();
	const TCHAR* pathTC = (const TCHAR*)pathVal;
	if (lineOffset == 0 || strcmp(activeDirName.get(), pathVal) || lineOffset != dirOffsetCounter) {
		errCode = f_opendir(&sxDIR, pathTC);
		if (errCode != FRESULT::FR_OK)
			goto errorFound;
		dirOffsetCounter = 0;
		activeDirName.set(pathVal);
		if (lineOffset > 0) {
			FILINFO fno;
			for (uint32_t ix = 0; ix < lineOffset; ++ix) {
				errCode = f_readdir(&sxDIR, &fno);
				if (errCode != FRESULT::FR_OK)
					break;
				if (fno.altname[0] == 0)
					break;
				dirOffsetCounter++;
			}
		}
	}
errorFound:;
	jWriter.reset();
	jWriter.setMemoryBased();
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^dir", false, true);
	jWriter.writeArrayStart("list", true, false);

	for (uint32_t ix = 0; ix < linesWanted; ++ix) {
		FILINFO fno;
		FRESULT err = f_readdir(&sxDIR, &fno);
		if (err != FRESULT::FR_OK)
			break;
		if (fno.altname[0] == 0)
			break;

		jWriter.writeOpeningTag(NULL, true);
		jWriter.writeAttribute("name", fno.fname);
		jWriter.writeAttribute("size", fno.fsize);
		jWriter.writeAttribute("date", fno.fdate);
		jWriter.writeAttribute("time", fno.ftime);

		// AM_RDO  0x01 Read only
		// AM_HID  0x02 Hidden
		// AM_SYS  0x04 System

		// AM_DIR  0x10 Directory
		// AM_ARC  0x20 Archive
		jWriter.writeAttribute("attr", fno.fattrib);

		jWriter.closeTag();
		dirOffsetCounter++;
	}
	jWriter.writeArrayEnding("list", true, false);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}

void smSysex::readBlock(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	uint32_t addr = 0;
	uint32_t size = blockBufferMax;
	int32_t fid = 0;

	auto repSN = reader.getReplySeqNum();
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "fid")) {
			fid = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "addr")) {
			addr = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "size")) {
			size = reader.readTagOrAttributeValueInt();
			if (size > blockBufferMax)
				size = blockBufferMax;
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	FILdata* fp = entryForFID(fid);
	FRESULT errCode = FR_OK;

	if (fp == nullptr) {
		errCode = FRESULT::FR_NOT_ENABLED;
	}
	UINT actuallyRead = 0;
	uint8_t* srcAddr = (uint8_t*)addr;
	if (errCode == FRESULT::FR_OK) {
		if (!readBlockBuffer && fp) {
			readBlockBuffer = (uint8_t*)GeneralMemoryAllocator::get().allocLowSpeed(blockBufferMax);
		}

		if (readBlockBuffer && fp) {
			noteFileIdUse(fp);
			// If file position requested is not what we expect, seek to requested.
			if (fp->fPosition != addr) {
				errCode = f_lseek(&fp->file, addr);
			}
			if (errCode == FRESULT::FR_OK) {
				errCode = f_read(&fp->file, readBlockBuffer, size, &actuallyRead);
				size = actuallyRead;
				srcAddr = readBlockBuffer;
				fp->fPosition = addr + actuallyRead;
			}
			else {
				D_PRINTLN("lseek issue: %d", errCode);
			}
		}
	}
	else {
		size = 0;
	}
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^read", false, true);
	jWriter.writeAttribute("fid", fid);
	jWriter.writeAttribute("addr", addr);
	jWriter.writeAttribute("size", size);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);

	jWriter.writeByte(0); // spacer between Json and encoded block.

	uint8_t working[8];
	if (size == 0) {
		D_PRINTLN("Read size 0");
	}
	for (uint32_t ix = 0; ix < size; ix += 7) {
		int pktSize = 7;
		if (ix + pktSize > size) {
			pktSize = size - ix;
		}
		uint8_t hiBits = 0;
		uint8_t rotBit = 1;
		for (int i = 1; i <= pktSize; ++i) {
			working[i] = (*srcAddr) & 0x7F;
			if ((*srcAddr) & 0x80) {
				hiBits |= rotBit;
			}
			srcAddr++;
			rotBit <<= 1;
		}
		working[0] = hiBits;
		jWriter.writeBlock(working, pktSize + 1);
	}
	sendMsg(cable, jWriter);
}

void smSysex::writeBlock(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	uint32_t fileId = 0;
	uint32_t addr = 0;
	uint32_t size = blockBufferMax;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "addr")) {
			addr = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "size")) {
			size = reader.readTagOrAttributeValueInt();
			if (size > blockBufferMax)
				size = blockBufferMax;
		}
		else if (!strcmp(tagName, "fid")) {
			fileId = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	if (!writeBlockBuffer) {
		writeBlockBuffer = (uint8_t*)GeneralMemoryAllocator::get().allocLowSpeed(blockBufferMax);
	}
	reader.match('}');
	reader.match('}'); // skip box too.

	// We should be on the separator character, check to make
	char aChar;
	if (reader.peekChar(&aChar) && aChar != 0) {
		D_PRINTLN("Missing Separater error in writeBlock");
	}
	uint32_t decodedSize = decodeDataFromReader(reader, writeBlockBuffer, size);
	D_PRINTLN("Decoded block len: %d", decodedSize);

	// Here is where we actually write the buffer out.
	FRESULT errCode = FRESULT::FR_OK;
	FILdata* fp = entryForFID(fileId);

	if (fp == nullptr) {
		errCode = FRESULT::FR_NOT_ENABLED;
	}
	if (writeBlockBuffer && (fp != nullptr)) {
		if (addr != fp->fPosition) {
			errCode = f_lseek(&fp->file, addr);
		}
		if (errCode == FRESULT::FR_OK) {
			noteFileIdUse(fp);
			UINT actuallyWritten = 0;
			errCode = f_write(&fp->file, writeBlockBuffer, decodedSize, &actuallyWritten);
			size = actuallyWritten;
			fp->fPosition = addr + actuallyWritten;
		}
	}
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^write", false, true);
	jWriter.writeAttribute("fid", fileId);
	jWriter.writeAttribute("addr", addr);
	jWriter.writeAttribute("size", size);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);

	sendMsg(cable, jWriter);
}

void smSysex::updateTime(MIDICable& cable, JsonDeserializer& reader) {

	FRESULT errCode;
	char const* tagName;
	uint32_t date = 0;
	uint32_t time = 0;
	String path;

	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(&path);
		}
		else if (!strcmp(tagName, "date")) {
			date = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "time")) {
			time = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	if (!path.isEmpty() && (date != 0 || time != 0)) {
		FILINFO finfo;
		finfo.fdate = date;
		finfo.ftime = time;
		const char* pathVal = path.get();
		const TCHAR* pathTC = (const TCHAR*)pathVal;
		errCode = f_utime(pathTC, &finfo);
	}
	else {
		errCode = FRESULT::FR_INVALID_PARAMETER;
	}
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^utime", false, true);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}

// A session ID or sid is a number used by clients to keep track of which messages belong to who.
void smSysex::assignSession(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	String tag;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "tag")) {
			reader.readTagOrAttributeValueString(&tag);
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	int sessionNum = 0;
	uint32_t minSeen = session_mono_counter;
	for (int i = 1; i <= MAX_SYSEX_SESSIONS; ++i) {
		if (sessionLRU_array[i] == 0) {
			sessionNum = i;
			break;
		}
		if (sessionLRU_array[i] < minSeen) {
			minSeen = sessionLRU_array[i];
			sessionNum = i;
		}
	}
	// Note the sessionNum as MRU to claim it.
	sessionLRU_array[sessionNum] = session_mono_counter++;

	startDirect(jWriter);
	jWriter.writeOpeningTag("^session", false, true);
	jWriter.writeAttribute("sid", sessionNum);
	jWriter.writeAttribute("tag", tag.get());
	jWriter.writeAttribute("midBase", sessionNum << SYSEX_SESSION_SHIFT);
	jWriter.writeAttribute("midMin", (sessionNum << SYSEX_SESSION_SHIFT) + 1);
	jWriter.writeAttribute("midMax", (sessionNum << SYSEX_SESSION_SHIFT) + SYSEX_MSGID_MAX);
	jWriter.writeAttribute("pipe", kMaxPipelinedRequests);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}

void smSysex::doPing(MIDICable& cable, JsonDeserializer& reader) {
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^ping", false, true);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}

static uint32_t crc32Sysex(const uint8_t* data, uint32_t len) {
	uint32_t crc = 0xFFFFFFFF;
	for (uint32_t i = 0; i < len; ++i) {
		crc ^= data[i];
		for (int b = 0; b < 8; ++b) {
			crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
		}
	}
	return ~crc;
}

// Transport diagnostic: reports what the device actually received, without touching the card.
// Reply carries the queued frame length, the encoded payload size as decodeDataFromReader would
// see it, a CRC32 over those payload bytes, and - if the host sent the self-describing ramp
// payload (byte i == i & 0x7F) - the first index where the received bytes departed from it.
void smSysex::doEcho(MIDICable& cable, JsonDeserializer& reader, int32_t frameLen) {
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		reader.exitTag();
	}
	reader.match('}');
	reader.match('}'); // skip box too.

	int32_t encodedSize = -1;
	uint32_t crc = 0;
	int32_t divergesAt = -1;
	char aChar;
	if (reader.peekChar(&aChar) && aChar == 0) {
		reader.readChar(&aChar);                           // skip the separator
		encodedSize = reader.bytesRemainingInBuffer() - 1; // don't count that 0xF7.
		const uint8_t* payload = (const uint8_t*)reader.GetCurrentAddressInBuffer();
		crc = crc32Sysex(payload, encodedSize);
		for (int32_t i = 0; i < encodedSize; ++i) {
			if (payload[i] != (i & 0x7F)) {
				divergesAt = i;
				break;
			}
		}
	}

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^echo", false, true);
	jWriter.writeAttribute("frameLen", frameLen);
	jWriter.writeAttribute("encoded", encodedSize);
	jWriter.writeAttributeHex("crc", crc, 8);
	jWriter.writeAttribute("rampDivergesAt", divergesAt);
	jWriter.writeAttribute("usbEvts", lastSysexRxEvents);
	jWriter.writeAttribute("usbLastCIN", lastSysexRxLastCIN);
	jWriter.writeAttribute("usbRxLen", lastSysexRxLen);
	jWriter.writeAttribute("usbBails", sysexRxOverflowBails);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}

// The `view` query reports the eight rows currently on the pad grid, for a sidecar display
// beside the Deluge. Keys are short and the reply is size-capped so it always fits in one
// SysEx frame: the client polls and has no way to ask for a continuation.
namespace {

/// Longest run of source characters copied out of any one name.
const uint32_t kMaxViewNameChars = 28;
/// Hosts are only dependably transparent up to a 752 byte frame, so stay under that.
const int32_t kMaxViewReplyBytes = 740;
/// What a row with no text at all costs, including the comma before it.
const int32_t kMinViewRowBytes = 46;
/// Room kept for "gen", the closing braces and the 0xF7.
const int32_t kViewReplyTailBytes = 26;

/// Copies up to `maxChars` characters of `src` into `dest` as 7-bit clean JSON string content.
/// Bytes with the high bit set become \u00XX, so a name using the Deluge's extended characters
/// can never put an 0xF7 or an 8-bit byte inside the SysEx frame. `dest` must hold
/// maxChars * 6 + 2 bytes.
void escapeForJson(char const* src, char* dest, uint32_t maxChars) {
	uint32_t written = 0;
	for (uint32_t i = 0; i < maxChars && src[i]; i++) {
		auto c = (uint8_t)src[i];
		if (c == '"' || c == '\\') {
			dest[written++] = '\\';
			dest[written++] = (char)c;
		}
		else if (c < 0x20 || c >= 0x80) {
			dest[written++] = '\\';
			dest[written++] = 'u';
			dest[written++] = '0';
			dest[written++] = '0';
			intToHex(c, &dest[written], 2);
			written += 2;
		}
		else {
			dest[written++] = (char)c;
		}
	}
	dest[written] = 0;
}

/// The name the OLED would show for this output: the user's name if it has one, otherwise the
/// generated one. Mirrors the name half of View::drawOutputNameFromDetails, but as text.
/// `buffer` is only used when a name has to be generated, and must hold 16 bytes.
char const* viewOutputName(Output* output, char* buffer) {
	if (!output->name.isEmpty()) {
		return output->name.get();
	}

	switch (output->type) {
	case OutputType::MIDI_OUT: {
		auto* instrument = (MIDIInstrument*)output;
		int32_t channel = instrument->getChannel();
		if (channel >= 16) {
			return (channel == MIDI_CHANNEL_MPE_LOWER_ZONE)   ? "MPE Lower"
			       : (channel == MIDI_CHANNEL_MPE_UPPER_ZONE) ? "MPE Upper"
			                                                  : "Transpose";
		}
		memcpy(buffer, "MIDI ", 5);
		slotToString(channel + 1, instrument->channelSuffix, &buffer[5], 1);
		return buffer;
	}

	case OutputType::CV: {
		memcpy(buffer, "CV ", 3);
		intToString(((NonAudioInstrument*)output)->getChannel() + 1, &buffer[3]);
		return buffer;
	}

	case OutputType::AUDIO:
		return getOutputTypeName(OutputType::AUDIO, (int32_t)((AudioOutput*)output)->mode);

	default:
		return getOutputTypeName(output->type, 0);
	}
}

char const* viewOutputTypeName(OutputType type) {
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
	default:
		return "none";
	}
}

char const* viewUIName(UIType uiType) {
	switch (uiType) {
	case UIType::SESSION:
		return "session";
	case UIType::ARRANGER:
		return "arranger";
	case UIType::INSTRUMENT_CLIP:
	case UIType::AUDIO_CLIP:
	case UIType::KEYBOARD_SCREEN:
	case UIType::AUTOMATION:
		return "clip";
	default:
		return "other";
	}
}

/// State bits reported as "s" for each row. There is deliberately no record-arm bit:
/// clips are armed for recording by default, so it would be set on nearly every row, and
/// the Deluge itself only surfaces it while the record button is held.
enum ViewRowState {
	VIEW_ROW_ACTIVE = 1,
	VIEW_ROW_SOLOED = 2,
	VIEW_ROW_ARMED = 4,
};

/// A colour actually present on that row of pads. The grid paints a track with the hue
/// stored for it in the song; the rows layout paints a clip from its own colour offset and
/// the pitch of its note rows, so pick the middle note row as the representative one - a row
/// of pads is several hues at once and any single answer is an approximation. Section colour
/// is no use here: a song living in one section would render eight identical bars.
RGB viewRowColour(Output* output, Clip* clip, bool gridLayout) {
	if (gridLayout && output->colour != 0) {
		return RGB::fromHue(output->colour);
	}

	if (clip && clip->type == ClipType::AUDIO) {
		return ((AudioClip*)clip)->getColour();
	}

	if (clip && clip->type == ClipType::INSTRUMENT) {
		auto* instrumentClip = (InstrumentClip*)clip;
		int32_t numNoteRows = instrumentClip->noteRows.getNumElements();
		if (numNoteRows > 0) {
			NoteRow* noteRow = instrumentClip->noteRows.getElement(numNoteRows / 2);
			// A kit's rows are coloured by row index, a melodic clip's by note.
			int32_t yNote = (output->type == OutputType::KIT) ? numNoteRows / 2 : noteRow->y;
			return instrumentClip->getMainColourFromY(yNote, noteRow->getColourOffset(instrumentClip));
		}
		return instrumentClip->getMainColourFromY(0, 0);
	}

	if (output->colour != 0) {
		return RGB::fromHue(output->colour);
	}
	return RGB::monochrome(160);
}

} // namespace

void smSysex::sendView(MIDICable& cable, JsonDeserializer& reader) {
	char escaped[kMaxViewNameChars * 6 + 2];
	char generated[16];

	UI* rootUI = getRootUI();
	UIType uiType = rootUI ? rootUI->getUIType() : UIType::NONE;
	bool isSession = (uiType == UIType::SESSION);
	bool isArranger = (uiType == UIType::ARRANGER);
	bool isGrid = isSession && currentSong->sessionLayout == SessionLayoutType::SessionLayoutTypeGrid;

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^view", false, true);
	jWriter.writeAttribute("ui", viewUIName(uiType), false);
	jWriter.writeAttribute("layout", isArranger ? "arranger" : (isGrid ? "grid" : "rows"), false);
	escapeForJson(currentSong->name.get(), escaped, kMaxViewNameChars);
	jWriter.writeAttribute("song", escaped, false);
	jWriter.writeAttribute("yScroll",
	                       isArranger ? currentSong->arrangementYScroll
	                                  : (isGrid ? currentSong->songGridScrollY : currentSong->songViewYScroll),
	                       false);
	jWriter.writeAttribute("xScroll", isGrid ? currentSong->songGridScrollX : 0, false);
	jWriter.writeAttribute("playing", playbackHandler.isEitherClockActive() ? 1 : 0, false);
	jWriter.writeArrayStart("rows", false, false);

	for (int32_t index = 0; index < kDisplayHeight; index++) {
		// Topmost pad row first, so the page can render the array straight down the screen.
		int32_t y = kDisplayHeight - 1 - index;
		int32_t rowsLeft = kDisplayHeight - index;
		int32_t room =
		    kMaxViewReplyBytes - jWriter.bytesWritten() - kViewReplyTailBytes - (rowsLeft - 1) * kMinViewRowBytes;

		Output* output = nullptr;
		Clip* clip = nullptr;
		if (isSession) {
			// In the grid layout the eight entries are the leftmost track columns, not pad rows.
			output = sessionView.getViewQueryRow(isGrid ? index : y, &clip);
		}
		else if (isArranger) {
			output = arrangerView.outputsOnScreen[y];
		}

		jWriter.writeOpeningTag(nullptr, false, false);
		jWriter.writeAttribute("y", isGrid ? index : y, false);

		if (!output) {
			jWriter.writeAttribute("t", "none", false);
			jWriter.closeTag();
			continue;
		}

		Clip* colourClip = clip ? clip : output->getActiveClip();
		uint32_t section = (colourClip && colourClip->section < kMaxNumSections) ? colourClip->section : 0;
		RGB colour = viewRowColour(output, colourClip, isGrid);

		uint32_t state = 0;
		if (clip) {
			state |= currentSong->isClipActive(clip) ? VIEW_ROW_ACTIVE : 0;
			state |= clip->soloingInSessionMode ? VIEW_ROW_SOLOED : 0;
			state |= (clip->armState != ArmState::OFF) ? VIEW_ROW_ARMED : 0;
		}
		else {
			bool soloed = output->soloingInArrangementMode;
			state |= soloed ? (VIEW_ROW_ACTIVE | VIEW_ROW_SOLOED) : 0;
			if (!currentSong->anyOutputsSoloingInArrangement && !output->mutedInArrangementMode) {
				state |= VIEW_ROW_ACTIVE;
			}
		}

		jWriter.writeAttribute("t", viewOutputTypeName(output->type), false);

		char const* name = viewOutputName(output, generated);
		int32_t textRoom = room - kMinViewRowBytes;
		uint32_t nameChars = std::min<int32_t>(std::max<int32_t>(textRoom, 0), kMaxViewNameChars);
		escapeForJson(name, escaped, nameChars);
		jWriter.writeAttribute("n", escaped, false);

		// The clip's own name is optional: it is usually unset, and it is the first thing to go
		// when the reply is running out of room.
		char const* clipName = clip ? clip->name.get() : "";
		if (*clipName) {
			int32_t clipRoom = textRoom - (int32_t)strlen(escaped) - 8;
			uint32_t clipChars = std::min<int32_t>(std::max<int32_t>(clipRoom, 0), kMaxViewNameChars);
			if (clipChars > 0) {
				escapeForJson(clipName, escaped, clipChars);
				jWriter.writeAttribute("c", escaped, false);
			}
		}

		intToHex(colour.r, &escaped[0], 2);
		intToHex(colour.g, &escaped[2], 2);
		intToHex(colour.b, &escaped[4], 2);
		jWriter.writeAttribute("k", escaped, false);
		jWriter.writeAttribute("s", (int32_t)state, false);
		if (colourClip) {
			jWriter.writeAttribute("x", (int32_t)section + 1, false);
		}
		jWriter.closeTag();
	}

	jWriter.writeArrayEnding("rows", false, false);

	// "gen" is written last so it can be a hash of everything before it: the page repaints only
	// when it changes, and nothing in the firmware has to know when the rows went stale.
	uint32_t gen = 2166136261u;
	char const* payload = jWriter.getBufferPtr();
	for (int32_t i = 7; i < jWriter.bytesWritten(); i++) {
		gen = (gen ^ (uint8_t)payload[i]) * 16777619u;
	}
	jWriter.writeAttribute("gen", (int32_t)(gen & 0x3FFFFFFF), false);

	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}

uint32_t smSysex::decodeDataFromReader(JsonDeserializer& reader, uint8_t* dest, uint32_t destMax) {
	char zip = 0;
	if (!reader.readChar(&zip) || zip) // skip separator, fail if not there.
		return 0;
	uint32_t encodedSize = reader.bytesRemainingInBuffer() - 1; // don't count that 0xF7.
	uint32_t amount = unpack_7bit_to_8bit(dest, destMax, (uint8_t*)reader.GetCurrentAddressInBuffer(), encodedSize);
	return amount;
}

void smSysex::sysexReceived(MIDICable& cable, uint8_t* data, int32_t len) {
	if (len < 3 || len > (int32_t)sysexBufferMax) {
		return;
	}

	SysExDataEntry& de = SysExQ.emplace_back(cable, len);
	memcpy(de.data, data, len);
}

void smSysex::handleNextSysEx() {

	if (SysExQ.empty())
		return;
	if (currentlyAccessingCard != 0)
		return;

	SysExDataEntry& de = SysExQ.front();

	// Backpressure: leave the request queued until its worst-case reply (a full ^read block, 7-bit
	// encoded plus JSON header) is guaranteed to fit in the cable's send buffer, so pipelined
	// clients self-throttle to the buffer's capacity instead of forcing reply drops and retries.
	// The defer cap lets a wedged link fall through to the send-side guard (drop + client retry).
	constexpr size_t kWorstCaseReplyBytes = sysexBufferMax + sysexBufferMax / 7 + 64;
	static uint32_t deferCount = 0;
	if (de.cable.sendBufferSpace() < kWorstCaseReplyBytes && deferCount < 4000) {
		deferCount++;
		return;
	}
	deferCount = 0;

	char const* tagName;
	uint8_t msgSeqNum = de.data[1];
	noteSessionIdUse(msgSeqNum);
	JsonDeserializer parser(de.data + 2, de.len - 2);
	parser.setReplySeqNum(msgSeqNum);

	parser.match('{');
	while (*(tagName = parser.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "open")) {
			openFile(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "close")) {
			closeFile(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "dir")) {
			getDirEntries(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "read")) {
			readBlock(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "write")) {
			writeBlock(de.cable, parser);
			goto done; // Already skipped end.
		}
		else if (!strcmp(tagName, "delete")) {
			deleteFile(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "mkdir")) {
			createDirectory(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "rename")) {
			rename(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "copy")) {
			copyFile(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "move")) {
			moveFile(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "utime")) {
			updateTime(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "session")) {
			assignSession(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "ping")) {
			doPing(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "echo")) {
			doEcho(de.cable, parser, de.len);
			goto done;
		}
		else if (!strcmp(tagName, "view")) {
			sendView(de.cable, parser);
			goto done;
		}
		parser.exitTag();
	}
done:
	SysExQ.pop_front();
}

// Helper function to parse file operation parameters
bool smSysex::parseFileOpParams(JsonDeserializer& reader, FileOpParams& params) {
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "from")) {
			reader.readTagOrAttributeValueString(&params.fromName);
		}
		else if (!strcmp(tagName, "to")) {
			reader.readTagOrAttributeValueString(&params.toName);
		}
		else if (!strcmp(tagName, "date")) {
			params.date = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "time")) {
			params.time = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	return params.getFromTC() && strlen(params.getFromTC()) && params.getToTC() && strlen(params.getToTC());
}

// Helper function to set file timestamp
void smSysex::setFileTimestamp(const TCHAR* path, uint32_t date, uint32_t time) {
	if (date != 0 || time != 0) {
		FILINFO finfo;
		finfo.fdate = date;
		finfo.ftime = time;
		f_utime(path, &finfo);
	}
}

// Helper function to perform file copy operation
FRESULT smSysex::performFileCopy(const FileOpParams& params) {
	FRESULT errCode = FRESULT::FR_OK;
	const TCHAR* fromTC = params.getFromTC();
	const TCHAR* toTC = params.getToTC();

	D_PRINTLN(fromTC);
	D_PRINTLN(toTC);

	// Create destination directory if needed
	bool pathCreateTried = false;
retry_copy:
	// Open source file for reading
	FIL srcFile, dstFile;
	errCode = f_open(&srcFile, fromTC, FA_READ);
	if (errCode == FRESULT::FR_OK) {
		// Open destination file for writing
		errCode = f_open(&dstFile, toTC, FA_WRITE | FA_CREATE_ALWAYS);
		if (errCode == FRESULT::FR_NO_PATH && !pathCreateTried) {
			// Try to create the destination directory
			// Don't set timestamps on directories - let them use current time
			String toNameCopy = params.toName;
			createPathDirectories(toNameCopy, 0, 0);
			pathCreateTried = true;
			f_close(&srcFile);
			goto retry_copy;
		}

		if (errCode == FRESULT::FR_OK) {
			// Copy file contents
			uint8_t* copyBuffer = nullptr;
			if (!readBlockBuffer) {
				readBlockBuffer = (uint8_t*)GeneralMemoryAllocator::get().allocLowSpeed(blockBufferMax);
			}
			copyBuffer = readBlockBuffer;

			if (copyBuffer) {
				UINT bytesRead, bytesWritten;
				do {
					errCode = f_read(&srcFile, copyBuffer, blockBufferMax, &bytesRead);
					if (errCode == FRESULT::FR_OK && bytesRead > 0) {
						errCode = f_write(&dstFile, copyBuffer, bytesRead, &bytesWritten);
						if (errCode != FRESULT::FR_OK || bytesWritten != bytesRead) {
							break;
						}
					}
				} while (errCode == FRESULT::FR_OK && bytesRead == blockBufferMax);
			}
			else {
				errCode = FRESULT::FR_NOT_ENOUGH_CORE;
			}

			f_close(&dstFile);

			// Set timestamp if provided and copy was successful
			if (errCode == FRESULT::FR_OK && params.hasTimestamp()) {
				setFileTimestamp(toTC, params.date, params.time);
			}
		}
		f_close(&srcFile);
	}

	return errCode;
}

void smSysex::copyFile(MIDICable& cable, JsonDeserializer& reader) {
	FileOpParams params;

	if (!parseFileOpParams(reader, params)) {
		return;
	}

	FRESULT errCode = performFileCopy(params);

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^copy", false, true);
	jWriter.writeAttribute("from", params.getFromPath());
	jWriter.writeAttribute("to", params.getToPath());
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}

void smSysex::moveFile(MIDICable& cable, JsonDeserializer& reader) {
	FileOpParams params;

	if (!parseFileOpParams(reader, params)) {
		return;
	}

	FRESULT errCode = FRESULT::FR_OK;
	const TCHAR* fromTC = params.getFromTC();
	const TCHAR* toTC = params.getToTC();

	D_PRINTLN(fromTC);
	D_PRINTLN(toTC);

	// Try rename first (works if source and destination are on same filesystem)
	errCode = f_rename(fromTC, toTC);

	// If rename failed due to missing path, try creating directories
	if (errCode == FRESULT::FR_NO_PATH) {
		// Don't set timestamps on directories - let them use current time
		String toNameCopy = params.toName;
		createPathDirectories(toNameCopy, 0, 0);
		errCode = f_rename(fromTC, toTC);
	}

	// If rename still fails (e.g., cross-filesystem move), fall back to copy+delete
	if (errCode != FRESULT::FR_OK) {
		// Use the shared copy function
		errCode = performFileCopy(params);

		// If copy was successful, delete the source file
		if (errCode == FRESULT::FR_OK) {
			FRESULT deleteResult = f_unlink(fromTC);

			// For move operation, both copy and delete must succeed
			if (deleteResult != FRESULT::FR_OK) {
				D_PRINTLN("Move: copy succeeded but delete failed: %d", deleteResult);
				// Clean up the destination file since move failed
				f_unlink(toTC);
				errCode = deleteResult;
			}
		}
	}
	else {
		// Rename was successful, set timestamp if provided
		if (params.hasTimestamp()) {
			setFileTimestamp(toTC, params.date, params.time);
		}
	}

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^move", false, true);
	jWriter.writeAttribute("from", params.getFromPath());
	jWriter.writeAttribute("to", params.getToPath());
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}
