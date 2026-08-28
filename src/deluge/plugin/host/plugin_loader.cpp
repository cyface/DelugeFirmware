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
		record.stage = PluginLoadStage::running;
		// First call into the blob: if the copy or the cache maintenance were wrong, this is where it shows, at
		// boot rather than in the middle of a song.
		record.checksum = selfCheck(plugin);
		record.builtinChecksum = selfCheck(*kBuiltinFxPlugins[index]);
		record.stage = PluginLoadStage::installed;
		fxSlotLoaded[index] = true;
		installLoadedFxPlugin(static_cast<uint32_t>(index), plugin);
		record.imageAddress = reinterpret_cast<uintptr_t>(image);
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
	record.stage = PluginLoadStage::running;
	record.checksum = selfCheck(loadedSource);
	record.builtinChecksum = selfCheck(kBuiltinDrumSourcePlugin);
	record.stage = PluginLoadStage::installed;
	sourceLoaded = true;
	installLoadedSourcePlugin(loadedSource);
	record.imageAddress = reinterpret_cast<uintptr_t>(image);
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

} // namespace

void loadPluginsFromCard() {
	numRecords = 0;
	if (f_opendir(&staticDIR.inner(), kPluginDirectory) != FR_OK) {
		return; // no PLUGINS/ on this card, which is the normal case
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
		record.status = loadOneFile(staticFNO.fname, record.fileSize, record);
		numRecords++;
		D_PRINTLN("plugin %s: name %s, status %d, %d bytes at 0x%08x, check 0x%08x vs built-in 0x%08x", record.file,
		          record.name, static_cast<int32_t>(record.status), record.imageSize, record.imageAddress,
		          record.checksum, record.builtinChecksum);
	}
	f_closedir(&staticDIR.inner());
}

std::span<const PluginLoadRecord> pluginLoadReport() {
	return {records, numRecords};
}

} // namespace deluge::plugin
