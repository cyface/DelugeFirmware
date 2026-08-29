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

#include "plugin/host/plugin_loader.h"
#include "OSLikeStuff/fault_handler/fault_handler.h"
#include "OSLikeStuff/scheduler_api.h"
#include "hid/display/display.h"
#include "io/debug/log.h"
#include "memory/memory_allocator_interface.h"
#include "plugin/host/plugin_host.h"
#include "plugin/plugin_blob.h"
#include "storage/storage_manager.h"
#include <cstring>
#include <string_view>

extern "C" {
#include "RZA1/cache/cache.h"
}

namespace deluge::plugin {

namespace {

constexpr char kPluginDirectory[] = "PLUGINS"; // an array, so the path buffer below can be sized from it

/// Names the plugin currently being run for the first time. Left behind on the card if that call never returns.
constexpr char kCanaryPath[] = "PLUGINS/CANARY.TXT";

PluginLoadRecord records[kMaxPluginFiles];
uint32_t numRecords = 0;

// Descriptors bound from blobs. The host is handed a pointer into these, so they have to outlive the scan - and
// they are the only mutable copy of a descriptor in the firmware; everything else is constexpr.
DelugeFxPlugin loadedFx[kNumBuiltinFxPlugins];
DelugeFxParamInfo loadedFxParams[kNumBuiltinFxPlugins][kMaxParamsPerFxPlugin];
bool fxSlotLoaded[kNumBuiltinFxPlugins];
DelugeSourcePlugin loadedSource;
DelugeSourceModelInfo loadedSourceModels[kMaxLoadedSourceModels];
bool sourceLoaded;

// What the user is told once the UI is up, if there is anything worth telling. Static because a popup keeps the
// pointer, and because this is written during the boot scan and read seconds later.
char noticeShort[8]; // 7-segment popup text; empty means the notice is a console line, not a popup
char noticeLong[48];

/// Seconds to wait before saying anything: the startup song is still loading, and a popup raised during setup()
/// is drawn over by the first thing the UI does.
constexpr double kNoticeDelay = 8.0;

void appendText(char* destination, uint32_t size, const char* source) {
	uint32_t length = 0;
	while (destination[length] != 0 && length + 1 < size) {
		length++;
	}
	for (; source != nullptr && *source != 0 && length + 1 < size; source++, length++) {
		destination[length] = *source;
	}
	destination[length] = 0;
}

/// Run seconds after boot, from the scheduler, because a popup raised during setup() is drawn over by the first
/// thing the UI does.
void showLoadNotice() {
	if (noticeLong[0] == 0) {
		return;
	}
	if (noticeShort[0] != 0) {
		const char* shortLong[2] = {noticeShort, noticeLong};
		display->displayPopup(shortLong);
	}
	else if (display->haveOLED()) {
		display->consoleText(noticeLong); // all is well: visible, but not in the way
	}
}

/// Leave a canary on the card naming the plugin about to be called for the first time. If the Deluge never comes
/// back from that call, the file is still there on the next boot and that plugin is left alone - so the boot loop
/// a bad blob could otherwise cause heals itself, without the user having to know about the BACK gesture. Same
/// idea as the startup-song canary in deluge.cpp.
void writeCanary(const char* fileName) {
	FIL file;
	if (f_open(&file, kCanaryPath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
		return;
	}
	UINT written = 0;
	f_write(&file, fileName, strlen(fileName), &written);
	f_close(&file);
}

void clearCanary() {
	f_unlink(kCanaryPath);
}

/// The plugin file named by a canary a previous boot left behind, or "" if that boot got through its plugins.
void readCanary(char* out, uint32_t size) {
	out[0] = 0;
	FIL file;
	if (f_open(&file, kCanaryPath, FA_READ) != FR_OK) {
		return;
	}
	UINT got = 0;
	if (f_read(&file, out, size - 1, &got) != FR_OK) {
		got = 0;
	}
	out[got] = 0;
	f_close(&file);
}

void copyName(char* destination, uint32_t size, const char* source) {
	uint32_t i = 0;
	for (; source != nullptr && source[i] != 0 && i + 1 < size; i++) {
		destination[i] = source[i];
	}
	destination[i] = 0;
}

/// A file this scan should look at: a .dlp that is not one of the AppleDouble twins ("._name.dlp") macOS leaves
/// behind whenever a card is written from a Mac. They are not plugins, and letting them fill the report would
/// hide the real files behind them.
bool looksLikeAPluginFile(const char* name) {
	const char* dot = strrchr(name, '.');
	return dot != nullptr && strcasecmp(dot, ".dlp") == 0 && name[0] != '.';
}

PluginLoadStatus statusFor(DelugePluginBlobStatus status) {
	switch (status) {
	case DELUGE_PLUGIN_BLOB_OK:
		return PluginLoadStatus::loaded;
	case DELUGE_PLUGIN_BLOB_BAD_MAGIC:
		return PluginLoadStatus::notAPlugin;
	case DELUGE_PLUGIN_BLOB_BAD_FORMAT:
		return PluginLoadStatus::badFormat;
	case DELUGE_PLUGIN_BLOB_BAD_ABI:
		return PluginLoadStatus::badAbi;
	default:
		// Bad size, kind, offset, entry point or CRC: whatever is on the card is not the file that was packed.
		return PluginLoadStatus::damaged;
	}
}

/// Make bytes the CPU just wrote fetchable as instructions from the same address: clean D and L2 so RAM holds
/// them, then invalidate I and the branch predictor so nothing stale is served. The #37 spike measured this as
/// the whole cost of running a blob from SDRAM - without it, execution is a lottery.
void makeCodeVisible(const void* start, uint32_t length) {
	auto begin = reinterpret_cast<uintptr_t>(start);
	invalidate_range_all_caches(begin, begin + length);
	L1_I_CacheFlushAll();
	__asm__ volatile("mcr p15, 0, %0, c7, c5, 6" ::"r"(0)); // BPIALL
	__asm__ volatile("dsb\nisb");
}

/// Which built-in insert FX a blob is offering to replace, or -1.
int32_t builtinFxIndexNamed(std::string_view name) {
	for (uint32_t i = 0; i < kNumBuiltinFxPlugins; i++) {
		if (name == kBuiltinFxPlugins[i]->name) {
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

/// A loaded FX takes over a built-in's slots in the param bank, and those slots' names, XML attributes and
/// defaults are baked into the firmware at compile time (fx_plugin_bank.h). So it may bring new DSP, but it has
/// to describe exactly the same params: anything else would make menus and saved songs lie.
bool loadedFxMatchesBuiltin(const DelugeFxPlugin& loaded, const DelugeFxPlugin& builtin) {
	if (loaded.numParams != builtin.numParams || loaded.stateSize > kFxPluginMaxStateBytes || loaded.reset == nullptr
	    || loaded.render == nullptr) {
		return false;
	}
	for (uint32_t i = 0; i < loaded.numParams; i++) {
		if (std::string_view{loaded.paramInfo[i].fileName} != builtin.paramInfo[i].fileName) {
			return false;
		}
	}
	return true;
}

/// Hand the plugin's address range to the fault handler, with the text the error screen should show. From here
/// on, a fault inside this plugin says so instead of freezing on an address nobody can place.
void nameTheImageForFaults(PluginLoadRecord& record, uint32_t imageSize) {
	record.faultText[0] = 0;
	appendText(record.faultText, sizeof(record.faultText), "PLUG "); // the 7-segment display shows these 4 letters
	appendText(record.faultText, sizeof(record.faultText), record.name);
	faultHandlerRegisterPluginImage(record.imageAddress, record.imageAddress + imageSize, record.faultText);
}

PluginLoadStatus bindAndInstall(const uint8_t* file, PluginLoadRecord& record) {
	const auto* header = reinterpret_cast<const DelugePluginBlobHeader*>(file);
	const void* image = file + header->imageOffset;
	const char* name = deluge_plugin_blob_string(file, header->fileSize, header->nameOffset);
	copyName(record.name, sizeof(record.name), name);
	record.kind = static_cast<uint8_t>(header->kind);
	record.abiVersion = static_cast<uint8_t>(header->abiVersion);
	record.imageSize = header->imageSize;

	if (header->kind == DELUGE_PLUGIN_BLOB_KIND_FX) {
		int32_t index = builtinFxIndexNamed(name);
		if (index < 0) {
			return PluginLoadStatus::unknown;
		}
		if (fxSlotLoaded[index]) {
			return PluginLoadStatus::duplicate;
		}
		DelugeFxPlugin& plugin = loadedFx[index];
		record.stage = PluginLoadStage::binding;
		if (deluge_plugin_blob_bind_fx(file, image, &plugin, loadedFxParams[index], kMaxParamsPerFxPlugin)
		    != DELUGE_PLUGIN_BLOB_OK) {
			return PluginLoadStatus::incompatible;
		}
		if (!loadedFxMatchesBuiltin(plugin, *kBuiltinFxPlugins[index])) {
			return PluginLoadStatus::incompatible;
		}
		record.stage = PluginLoadStage::revealing;
		makeCodeVisible(image, header->imageSize);
		record.imageAddress = reinterpret_cast<uintptr_t>(image);
		nameTheImageForFaults(record, header->imageSize); // before the first call, so that call is attributable
		record.stage = PluginLoadStage::running;
		// First call into the blob: if the copy or the cache maintenance were wrong, this is where it shows, at
		// boot rather than in the middle of a song. The canary is what makes that survivable.
		writeCanary(record.file);
		record.checksum = selfCheck(plugin);
		record.builtinChecksum = selfCheck(*kBuiltinFxPlugins[index]);
		clearCanary();
		record.stage = PluginLoadStage::installed;
		fxSlotLoaded[index] = true;
		installLoadedFxPlugin(static_cast<uint32_t>(index), plugin);
		return PluginLoadStatus::loaded;
	}

	// A source plugin: today the one behind OscType::DRUM, matched by name like an FX is.
	if (std::string_view{name} != kBuiltinDrumSourcePlugin.name) {
		return PluginLoadStatus::unknown;
	}
	if (sourceLoaded) {
		return PluginLoadStatus::duplicate;
	}
	record.stage = PluginLoadStage::binding;
	if (deluge_plugin_blob_bind_source(file, image, &loadedSource, loadedSourceModels, kMaxLoadedSourceModels)
	    != DELUGE_PLUGIN_BLOB_OK) {
		return PluginLoadStatus::incompatible;
	}
	// Unlike an FX, a source plugin owns its whole menu: it may bring its own models and macro names, so only the
	// host's own limits are checked here.
	if (!sourcePluginFitsTheHost(loadedSource)) {
		return PluginLoadStatus::incompatible;
	}
	record.stage = PluginLoadStage::revealing;
	makeCodeVisible(image, header->imageSize);
	record.imageAddress = reinterpret_cast<uintptr_t>(image);
	nameTheImageForFaults(record, header->imageSize); // before the first call, so that call is attributable
	record.stage = PluginLoadStage::running;
	writeCanary(record.file);
	record.checksum = selfCheck(loadedSource);
	record.builtinChecksum = selfCheck(kBuiltinDrumSourcePlugin);
	clearCanary();
	record.stage = PluginLoadStage::installed;
	sourceLoaded = true;
	installLoadedSourcePlugin(loadedSource);
	return PluginLoadStatus::loaded;
}

/// Read one file into SDRAM and, if everything about it checks out, put it to work. The buffer stays allocated
/// for the run either way it is used: the descriptor's names point into it.
PluginLoadStatus loadOneFile(const char* fileName, uint32_t fileSize, PluginLoadRecord& record) {
	if (fileSize < sizeof(DelugePluginBlobHeader)) {
		return PluginLoadStatus::notAPlugin;
	}
	if (fileSize > kMaxPluginFileBytes) {
		return PluginLoadStatus::tooBig;
	}
	// SDRAM, because that is where runtime-loaded code may execute from, and 32-byte aligned so the image inside
	// the file (itself at a multiple of 32) lands on a cache line - what the maintenance below works in.
	void* allocation = allocLowSpeed(fileSize + DELUGE_PLUGIN_BLOB_IMAGE_ALIGN);
	if (allocation == nullptr) {
		return PluginLoadStatus::noMemory;
	}
	auto address = reinterpret_cast<uintptr_t>(allocation);
	address =
	    (address + DELUGE_PLUGIN_BLOB_IMAGE_ALIGN - 1) & ~static_cast<uintptr_t>(DELUGE_PLUGIN_BLOB_IMAGE_ALIGN - 1);
	auto* buffer = reinterpret_cast<uint8_t*>(address);

	char path[sizeof(kPluginDirectory) + sizeof(staticFNO.fname) + 2];
	strcpy(path, kPluginDirectory);
	strcat(path, "/");
	strncat(path, fileName, sizeof(path) - strlen(path) - 1);

	FIL file;
	record.stage = PluginLoadStage::reading;
	PluginLoadStatus status = PluginLoadStatus::unreadable;
	if (f_open(&file, path, FA_READ) == FR_OK) {
		UINT got = 0;
		FRESULT result = f_read(&file, buffer, fileSize, &got);
		f_close(&file);
		if (result == FR_OK && got == fileSize) {
			record.stage = PluginLoadStage::validating;
			status = statusFor(deluge_plugin_blob_validate(buffer, fileSize));
			if (status == PluginLoadStatus::loaded) {
				status = bindAndInstall(buffer, record);
			}
		}
	}
	if (status != PluginLoadStatus::loaded) {
		delugeDealloc(allocation);
	}
	return status;
}

/// Tell the user what happened, if it is worth telling: a plugin that did not load is the whole reason the ABI
/// version is in the file, and saying nothing would leave them wondering why the Deluge sounds like it did before.
void announce() {
	uint32_t loaded = 0;
	const PluginLoadRecord* problem = nullptr;
	bool safeBoot = false;
	for (uint32_t i = 0; i < numRecords; i++) {
		if (records[i].status == PluginLoadStatus::loaded) {
			loaded++;
		}
		else if (records[i].status == PluginLoadStatus::skipped) {
			safeBoot = true;
		}
		else if (problem == nullptr) {
			problem = &records[i];
		}
	}
	noticeShort[0] = 0;
	noticeLong[0] = 0;
	if (safeBoot) {
		appendText(noticeShort, sizeof(noticeShort), "SKIP");
		appendText(noticeLong, sizeof(noticeLong), "Plugins skipped");
	}
	else if (problem != nullptr) {
		char subject[sizeof(problem->file)];
		describeSubject(*problem, subject, sizeof(subject));
		if (problem->status == PluginLoadStatus::crashedBefore) {
			appendText(noticeShort, sizeof(noticeShort), "CRSH");
			appendText(noticeLong, sizeof(noticeLong), "Plugin crashed, skipped: ");
		}
		else {
			appendText(noticeShort, sizeof(noticeShort), "PLUG");
			appendText(noticeLong, sizeof(noticeLong), "Plugin not loaded: ");
		}
		appendText(noticeLong, sizeof(noticeLong), subject);
	}
	else if (loaded > 0) {
		// A console line, no popup: worth confirming, not worth interrupting for.
		appendText(noticeLong, sizeof(noticeLong), loaded == 1 ? "1 plugin loaded" : "Plugins loaded");
	}
	else {
		return; // nothing on the card at all, which is the normal case and not worth a word
	}
	addOnceTask(showLoadNotice, 100, kNoticeDelay, "plugin load notice", RESOURCE_NONE);
}

} // namespace

void loadPluginsFromCard(bool safeBoot) {
	numRecords = 0;
	if (safeBoot) {
		// BACK was held at power-on. Nothing is read, so a plugin that crashes the Deluge cannot keep it from
		// booting - which is the only state in which the user has no other way out.
		PluginLoadRecord& record = records[numRecords++];
		record = PluginLoadRecord{};
		copyName(record.file, sizeof(record.file), kPluginDirectory);
		record.status = PluginLoadStatus::skipped;
		D_PRINTLN("plugins skipped: safe boot");
		announce();
		return;
	}
	if (f_opendir(&staticDIR.inner(), kPluginDirectory) != FR_OK) {
		return; // no PLUGINS/ on this card, which is the normal case
	}
	// Whatever the last boot was running when it stopped. Cleared now rather than after the scan, so a plugin is
	// only ever skipped for one boot: the user may have replaced the file, and we should let it try again.
	char crashedFile[sizeof(records[0].file)];
	readCanary(crashedFile, sizeof(crashedFile));
	if (crashedFile[0] != 0) {
		clearCanary();
	}
	while (numRecords < kMaxPluginFiles) {
		if (f_readdir(&staticDIR.inner(), &staticFNO) != FR_OK || staticFNO.fname[0] == 0) {
			break;
		}
		if ((staticFNO.fattrib & AM_DIR) != 0 || !looksLikeAPluginFile(staticFNO.fname)) {
			continue;
		}
		PluginLoadRecord& record = records[numRecords];
		record = PluginLoadRecord{};
		copyName(record.file, sizeof(record.file), staticFNO.fname);
		record.fileSize = static_cast<uint32_t>(staticFNO.fsize);
		if (crashedFile[0] != 0 && strcasecmp(staticFNO.fname, crashedFile) == 0) {
			record.status = PluginLoadStatus::crashedBefore;
		}
		else {
			record.status = loadOneFile(staticFNO.fname, record.fileSize, record);
		}
		numRecords++;
		D_PRINTLN("plugin %s: name %s, status %d, %d bytes at 0x%08x, check 0x%08x vs built-in 0x%08x", record.file,
		          record.name, static_cast<int32_t>(record.status), record.imageSize, record.imageAddress,
		          record.checksum, record.builtinChecksum);
	}
	f_closedir(&staticDIR.inner());
	announce();
}

const char* describe(PluginLoadStatus status) {
	switch (status) {
	case PluginLoadStatus::loaded:
		return "loaded";
	case PluginLoadStatus::unreadable:
		return "unreadable";
	case PluginLoadStatus::tooBig:
		return "too big";
	case PluginLoadStatus::noMemory:
		return "no memory";
	case PluginLoadStatus::notAPlugin:
		return "not a plugin";
	case PluginLoadStatus::badFormat:
		return "newer format";
	case PluginLoadStatus::badAbi:
		return "wrong ABI";
	case PluginLoadStatus::damaged:
		return "damaged";
	case PluginLoadStatus::unknown:
		return "unknown name";
	case PluginLoadStatus::incompatible:
		return "incompatible";
	case PluginLoadStatus::duplicate:
		return "duplicate";
	case PluginLoadStatus::skipped:
		return "safe boot";
	case PluginLoadStatus::crashedBefore:
		return "crashed";
	}
	return "?";
}

void describeSubject(const PluginLoadRecord& record, char* out, uint32_t size) {
	if (record.name[0] != 0) {
		copyName(out, size, record.name);
		return;
	}
	copyName(out, size, record.file);
	uint32_t length = 0;
	while (out[length] != 0) {
		length++;
	}
	if (length > 4 && strcasecmp(&out[length - 4], ".dlp") == 0) {
		out[length - 4] = 0;
	}
}

std::span<const PluginLoadRecord> pluginLoadReport() {
	return {records, numRecords};
}

} // namespace deluge::plugin
