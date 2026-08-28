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

/*
 * Deluge plugin blob (.dlp): the on-card container for a plugin kernel, tier 2.
 *
 * A .dlp is a plugin_abi.h descriptor turned into position-independent data. Everything the host reads as a
 * pointer at runtime - names, param / model tables, entry points - is a *offset* here, because a blob is copied
 * to whatever SDRAM address is free and nothing in it may need a load address:
 *
 *   +0  DelugePluginBlobHeader     magic, format / ABI version, kind, sizes, CRC
 *       DelugeFxBlobDesc |         the descriptor, offsets only
 *       DelugeSourceBlobDesc
 *       param / model tables       arrays of offsets
 *       string table               NUL-terminated names, offsets are file-relative
 *   +imageOffset                   .text + .rodata exactly as linked (plugin.ld), the only part that gets
 *                                  copied into executable SDRAM; entry points are offsets into it
 *
 * The loader keeps the whole file resident (the descriptor it hands the host points at the strings in it) and
 * copies [imageOffset, imageOffset + imageSize) to a DELUGE_PLUGIN_BLOB_IMAGE_ALIGN-aligned address, after which
 * deluge_plugin_blob_bind_fx() / _bind_source() build the ordinary DelugeFxPlugin / DelugeSourcePlugin the rest of
 * the firmware already consumes. Nothing here allocates or reads the card: the caller owns every buffer.
 *
 * Plain freestanding C (no libc), so the offline packer's verifier and the firmware loader parse a blob with the
 * same code. Multi-byte fields are little-endian, the byte order of both the tool host and the target.
 */
#pragma once
#include "plugin/plugin_abi.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DELUGE_PLUGIN_BLOB_MAGIC 0x42504C44u /* "DLPB" little-endian */

/* Bump when the layout below changes. Separate from DELUGE_PLUGIN_ABI_VERSION: the container can grow a field
 * without the DSP contract moving, and the DSP contract can move without the container changing shape. */
#define DELUGE_PLUGIN_BLOB_FORMAT_VERSION 1u

/* Alignment the image must be given in memory (a cache line: the loader cleans and invalidates by range). */
#define DELUGE_PLUGIN_BLOB_IMAGE_ALIGN 32u

/* Which descriptor follows the header. */
enum {
	DELUGE_PLUGIN_BLOB_KIND_FX = 1u,     /* DelugeFxBlobDesc     -> DelugeFxPlugin */
	DELUGE_PLUGIN_BLOB_KIND_SOURCE = 2u, /* DelugeSourceBlobDesc -> DelugeSourcePlugin */
};

typedef struct {
	uint32_t magic;         /* DELUGE_PLUGIN_BLOB_MAGIC */
	uint32_t formatVersion; /* DELUGE_PLUGIN_BLOB_FORMAT_VERSION */
	uint32_t abiVersion;    /* DELUGE_PLUGIN_ABI_VERSION the kernel was built against */
	uint32_t kind;          /* DELUGE_PLUGIN_BLOB_KIND_* */
	uint32_t fileSize;      /* total bytes of the file */
	uint32_t crc32;         /* CRC-32 (IEEE, reflected) of the whole file with these four bytes read as zero */
	uint32_t imageOffset;   /* .text + .rodata; DELUGE_PLUGIN_BLOB_IMAGE_ALIGN-aligned within the file */
	uint32_t imageSize;
	uint32_t descOffset; /* DelugeFxBlobDesc or DelugeSourceBlobDesc, by kind */
	uint32_t descSize;
	uint32_t nameOffset;  /* display name of the plugin (string table) */
	uint32_t toolVersion; /* stamp of the packer that wrote the file; informational only */
} DelugePluginBlobHeader;

/* One entry of an insert FX's param table: the three names as string-table offsets, plus the raw q31 default. */
typedef struct {
	uint32_t name;
	uint32_t shortName;
	uint32_t fileName;
	int32_t defaultValue;
} DelugeFxBlobParam;

/* Entry points are offsets into the image with the Thumb bit set, exactly as the ELF symbol carries them, so the
 * host's function pointer is imageBase + entry. An optional entry point is 0. */
typedef struct {
	uint32_t numParams;
	uint32_t paramsOffset; /* DelugeFxBlobParam[numParams] */
	uint32_t stateSize;
	uint32_t resetEntry;
	uint32_t isActiveEntry; /* 0 = the FX is always active */
	uint32_t renderEntry;
} DelugeFxBlobDesc;

/* A DelugePluginName as offsets; a macro slot the model does not use has both offsets 0. */
typedef struct {
	uint32_t name;
	uint32_t shortName;
} DelugeBlobName;

typedef struct {
	DelugeBlobName name;
	uint32_t fileName;
	DelugeBlobName macros[DELUGE_SOURCE_PLUGIN_MAX_MACROS];
} DelugeSourceBlobModel;

typedef struct {
	uint32_t numModels;
	uint32_t modelsOffset; /* DelugeSourceBlobModel[numModels] */
	uint32_t numMacros;
	uint32_t voiceStateSize;
	uint32_t scratchSize;
	uint32_t maxBlockSize;
	uint32_t initEntry;
	uint32_t triggerEntry;
	uint32_t renderEntry;
} DelugeSourceBlobDesc;

/* Why a blob was rejected. Everything but OK means "fall back to the built-in and say so". */
typedef enum {
	DELUGE_PLUGIN_BLOB_OK = 0,
	DELUGE_PLUGIN_BLOB_BAD_MAGIC,  /* not a .dlp at all */
	DELUGE_PLUGIN_BLOB_BAD_FORMAT, /* container laid out by a version this host does not know */
	DELUGE_PLUGIN_BLOB_BAD_ABI,    /* built against a different plugin_abi.h */
	DELUGE_PLUGIN_BLOB_BAD_SIZE,   /* truncated, or fileSize disagrees with the bytes read */
	DELUGE_PLUGIN_BLOB_BAD_KIND,   /* unknown kind, or not the kind the caller asked to bind */
	DELUGE_PLUGIN_BLOB_BAD_OFFSET, /* a table, string or the image lies outside the file */
	DELUGE_PLUGIN_BLOB_BAD_ENTRY,  /* an entry point is missing, or outside the image */
	DELUGE_PLUGIN_BLOB_BAD_CRC,    /* the bytes do not match the CRC in the header */
	DELUGE_PLUGIN_BLOB_TOO_MANY,   /* more params / models than the caller's storage holds */
} DelugePluginBlobStatus;

/* CRC-32 (IEEE 802.3, reflected, init and final xor 0xFFFFFFFF) over `size` bytes, treating the four bytes at
 * `skipOffset` as zero (pass size for skipOffset to hash everything). Bitwise: no table to place, and a few
 * hundred microseconds on the largest blob a card is going to hold. */
static inline uint32_t deluge_plugin_blob_crc32(const void* data, uint32_t size, uint32_t skipOffset) {
	const uint8_t* bytes = (const uint8_t*)data;
	uint32_t crc = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < size; i++) {
		uint32_t byte = (i >= skipOffset && i < skipOffset + 4u) ? 0u : bytes[i];
		crc ^= byte;
		for (uint32_t bit = 0; bit < 8u; bit++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
		}
	}
	return ~crc;
}

/* Is [offset, offset + length) inside a file of `size` bytes? Written so no addition can overflow. */
static inline int32_t deluge_plugin_blob_in_file(uint32_t size, uint32_t offset, uint32_t length) {
	return offset <= size && length <= size - offset;
}

/* A string-table offset that resolves to a NUL-terminated string inside the file. Offset 0 means "none": it can
 * never be a real string because the header sits there. */
static inline const char* deluge_plugin_blob_string(const void* file, uint32_t size, uint32_t offset) {
	if (offset == 0 || offset >= size) {
		return (const char*)0;
	}
	const char* text = (const char*)file + offset;
	for (uint32_t i = offset; i < size; i++) {
		if (((const uint8_t*)file)[i] == 0) {
			return text;
		}
	}
	return (const char*)0; /* runs off the end of the file */
}

static inline int32_t deluge_plugin_blob_entry_ok(const DelugePluginBlobHeader* header, uint32_t entry,
                                                  int32_t optional) {
	if (entry == 0) {
		return optional;
	}
	/* Thumb-2, like the firmware: the ELF symbol value carries bit 0 set and the host calls imageBase + entry. */
	return (entry & 1u) != 0 && (entry & ~1u) < header->imageSize;
}

/* Check a whole file: shape, CRC, and that every offset it contains lands inside it. A blob that passes may be
 * bound without further bounds checks. `size` is how many bytes were actually read. */
static inline DelugePluginBlobStatus deluge_plugin_blob_validate(const void* file, uint32_t size) {
	if (size < sizeof(DelugePluginBlobHeader)) {
		return DELUGE_PLUGIN_BLOB_BAD_SIZE;
	}
	const DelugePluginBlobHeader* header = (const DelugePluginBlobHeader*)file;
	if (header->magic != DELUGE_PLUGIN_BLOB_MAGIC) {
		return DELUGE_PLUGIN_BLOB_BAD_MAGIC;
	}
	if (header->formatVersion != DELUGE_PLUGIN_BLOB_FORMAT_VERSION) {
		return DELUGE_PLUGIN_BLOB_BAD_FORMAT;
	}
	if (header->fileSize != size) {
		return DELUGE_PLUGIN_BLOB_BAD_SIZE;
	}
	if (header->abiVersion != DELUGE_PLUGIN_ABI_VERSION) {
		return DELUGE_PLUGIN_BLOB_BAD_ABI;
	}
	if (!deluge_plugin_blob_in_file(size, header->imageOffset, header->imageSize) || header->imageSize == 0
	    || (header->imageOffset % DELUGE_PLUGIN_BLOB_IMAGE_ALIGN) != 0) {
		return DELUGE_PLUGIN_BLOB_BAD_OFFSET;
	}
	if (!deluge_plugin_blob_in_file(size, header->descOffset, header->descSize) || (header->descOffset % 4u) != 0) {
		return DELUGE_PLUGIN_BLOB_BAD_OFFSET;
	}
	if (deluge_plugin_blob_string(file, size, header->nameOffset) == (const char*)0) {
		return DELUGE_PLUGIN_BLOB_BAD_OFFSET;
	}
	if (deluge_plugin_blob_crc32(file, size, (uint32_t)__builtin_offsetof(DelugePluginBlobHeader, crc32))
	    != header->crc32) {
		return DELUGE_PLUGIN_BLOB_BAD_CRC;
	}

	if (header->kind == DELUGE_PLUGIN_BLOB_KIND_FX) {
		if (header->descSize < sizeof(DelugeFxBlobDesc)) {
			return DELUGE_PLUGIN_BLOB_BAD_OFFSET;
		}
		const DelugeFxBlobDesc* desc = (const DelugeFxBlobDesc*)((const uint8_t*)file + header->descOffset);
		if (desc->numParams > size / sizeof(DelugeFxBlobParam)
		    || !deluge_plugin_blob_in_file(size, desc->paramsOffset, desc->numParams * sizeof(DelugeFxBlobParam))
		    || (desc->paramsOffset % 4u) != 0) {
			return DELUGE_PLUGIN_BLOB_BAD_OFFSET;
		}
		const DelugeFxBlobParam* params = (const DelugeFxBlobParam*)((const uint8_t*)file + desc->paramsOffset);
		for (uint32_t i = 0; i < desc->numParams; i++) {
			if (deluge_plugin_blob_string(file, size, params[i].name) == (const char*)0
			    || deluge_plugin_blob_string(file, size, params[i].shortName) == (const char*)0
			    || deluge_plugin_blob_string(file, size, params[i].fileName) == (const char*)0) {
				return DELUGE_PLUGIN_BLOB_BAD_OFFSET;
			}
		}
		if (!deluge_plugin_blob_entry_ok(header, desc->resetEntry, 0)
		    || !deluge_plugin_blob_entry_ok(header, desc->isActiveEntry, 1)
		    || !deluge_plugin_blob_entry_ok(header, desc->renderEntry, 0)) {
			return DELUGE_PLUGIN_BLOB_BAD_ENTRY;
		}
		return DELUGE_PLUGIN_BLOB_OK;
	}

	if (header->kind == DELUGE_PLUGIN_BLOB_KIND_SOURCE) {
		if (header->descSize < sizeof(DelugeSourceBlobDesc)) {
			return DELUGE_PLUGIN_BLOB_BAD_OFFSET;
		}
		const DelugeSourceBlobDesc* desc = (const DelugeSourceBlobDesc*)((const uint8_t*)file + header->descOffset);
		if (desc->numModels == 0 || desc->numModels > size / sizeof(DelugeSourceBlobModel)
		    || !deluge_plugin_blob_in_file(size, desc->modelsOffset, desc->numModels * sizeof(DelugeSourceBlobModel))
		    || (desc->modelsOffset % 4u) != 0 || desc->numMacros > DELUGE_SOURCE_PLUGIN_MAX_MACROS) {
			return DELUGE_PLUGIN_BLOB_BAD_OFFSET;
		}
		const DelugeSourceBlobModel* models = (const DelugeSourceBlobModel*)((const uint8_t*)file + desc->modelsOffset);
		for (uint32_t i = 0; i < desc->numModels; i++) {
			if (deluge_plugin_blob_string(file, size, models[i].name.name) == (const char*)0
			    || deluge_plugin_blob_string(file, size, models[i].name.shortName) == (const char*)0
			    || deluge_plugin_blob_string(file, size, models[i].fileName) == (const char*)0) {
				return DELUGE_PLUGIN_BLOB_BAD_OFFSET;
			}
			for (uint32_t m = 0; m < DELUGE_SOURCE_PLUGIN_MAX_MACROS; m++) {
				uint32_t name = models[i].macros[m].name;
				uint32_t shortName = models[i].macros[m].shortName;
				int32_t used = (m < desc->numMacros) && (name != 0 || shortName != 0);
				if (used
				    && (deluge_plugin_blob_string(file, size, name) == (const char*)0
				        || deluge_plugin_blob_string(file, size, shortName) == (const char*)0)) {
					return DELUGE_PLUGIN_BLOB_BAD_OFFSET;
				}
			}
		}
		if (!deluge_plugin_blob_entry_ok(header, desc->initEntry, 0)
		    || !deluge_plugin_blob_entry_ok(header, desc->triggerEntry, 0)
		    || !deluge_plugin_blob_entry_ok(header, desc->renderEntry, 0)) {
			return DELUGE_PLUGIN_BLOB_BAD_ENTRY;
		}
		return DELUGE_PLUGIN_BLOB_OK;
	}

	return DELUGE_PLUGIN_BLOB_BAD_KIND;
}

/* imageBase + entry as a callable pointer. `imageBase` is where the image was copied, not the file buffer. */
static inline void* deluge_plugin_blob_entry(const void* imageBase, uint32_t entry) {
	return entry == 0 ? (void*)0 : (void*)((uintptr_t)imageBase + entry);
}

/* Fill in the DelugeFxPlugin the host consumes. `file` must have passed deluge_plugin_blob_validate(); `imageBase`
 * is where its image now lives, executable; `paramStorage` (caller-owned, and it must outlive the plugin) receives
 * the param table. The strings are left pointing into `file`, so that buffer must stay resident too. */
static inline DelugePluginBlobStatus deluge_plugin_blob_bind_fx(const void* file, const void* imageBase,
                                                                DelugeFxPlugin* plugin, DelugeFxParamInfo* paramStorage,
                                                                uint32_t maxParams) {
	const DelugePluginBlobHeader* header = (const DelugePluginBlobHeader*)file;
	if (header->kind != DELUGE_PLUGIN_BLOB_KIND_FX) {
		return DELUGE_PLUGIN_BLOB_BAD_KIND;
	}
	const DelugeFxBlobDesc* desc = (const DelugeFxBlobDesc*)((const uint8_t*)file + header->descOffset);
	if (desc->numParams > maxParams) {
		return DELUGE_PLUGIN_BLOB_TOO_MANY;
	}
	const DelugeFxBlobParam* params = (const DelugeFxBlobParam*)((const uint8_t*)file + desc->paramsOffset);
	uint32_t size = header->fileSize;
	for (uint32_t i = 0; i < desc->numParams; i++) {
		paramStorage[i].name = deluge_plugin_blob_string(file, size, params[i].name);
		paramStorage[i].shortName = deluge_plugin_blob_string(file, size, params[i].shortName);
		paramStorage[i].fileName = deluge_plugin_blob_string(file, size, params[i].fileName);
		paramStorage[i].defaultValue = params[i].defaultValue;
	}
	plugin->abiVersion = header->abiVersion;
	plugin->name = deluge_plugin_blob_string(file, size, header->nameOffset);
	plugin->numParams = desc->numParams;
	plugin->paramInfo = paramStorage;
	plugin->stateSize = desc->stateSize;
	plugin->reset = (DelugeFxResetFn)deluge_plugin_blob_entry(imageBase, desc->resetEntry);
	plugin->isActive = (DelugeFxIsActiveFn)deluge_plugin_blob_entry(imageBase, desc->isActiveEntry);
	plugin->render = (DelugeFxRenderFn)deluge_plugin_blob_entry(imageBase, desc->renderEntry);
	return DELUGE_PLUGIN_BLOB_OK;
}

/* The source-plugin twin of deluge_plugin_blob_bind_fx(): same ownership rules, `modelStorage` receives the model
 * table (names, XML file names and per-model macro labels all pointing into `file`). */
static inline DelugePluginBlobStatus deluge_plugin_blob_bind_source(const void* file, const void* imageBase,
                                                                    DelugeSourcePlugin* plugin,
                                                                    DelugeSourceModelInfo* modelStorage,
                                                                    uint32_t maxModels) {
	const DelugePluginBlobHeader* header = (const DelugePluginBlobHeader*)file;
	if (header->kind != DELUGE_PLUGIN_BLOB_KIND_SOURCE) {
		return DELUGE_PLUGIN_BLOB_BAD_KIND;
	}
	const DelugeSourceBlobDesc* desc = (const DelugeSourceBlobDesc*)((const uint8_t*)file + header->descOffset);
	if (desc->numModels > maxModels) {
		return DELUGE_PLUGIN_BLOB_TOO_MANY;
	}
	const DelugeSourceBlobModel* models = (const DelugeSourceBlobModel*)((const uint8_t*)file + desc->modelsOffset);
	uint32_t size = header->fileSize;
	for (uint32_t i = 0; i < desc->numModels; i++) {
		modelStorage[i].name.name = deluge_plugin_blob_string(file, size, models[i].name.name);
		modelStorage[i].name.shortName = deluge_plugin_blob_string(file, size, models[i].name.shortName);
		modelStorage[i].fileName = deluge_plugin_blob_string(file, size, models[i].fileName);
		for (uint32_t m = 0; m < DELUGE_SOURCE_PLUGIN_MAX_MACROS; m++) {
			modelStorage[i].macros[m].name = deluge_plugin_blob_string(file, size, models[i].macros[m].name);
			modelStorage[i].macros[m].shortName = deluge_plugin_blob_string(file, size, models[i].macros[m].shortName);
		}
	}
	plugin->abiVersion = header->abiVersion;
	plugin->name = deluge_plugin_blob_string(file, size, header->nameOffset);
	plugin->numModels = desc->numModels;
	plugin->modelInfo = modelStorage;
	plugin->numMacros = desc->numMacros;
	plugin->voiceStateSize = desc->voiceStateSize;
	plugin->scratchSize = desc->scratchSize;
	plugin->maxBlockSize = desc->maxBlockSize;
	plugin->init = (DelugeSourceInitFn)deluge_plugin_blob_entry(imageBase, desc->initEntry);
	plugin->trigger = (DelugeSourceTriggerFn)deluge_plugin_blob_entry(imageBase, desc->triggerEntry);
	plugin->render = (DelugeSourceRenderFn)deluge_plugin_blob_entry(imageBase, desc->renderEntry);
	return DELUGE_PLUGIN_BLOB_OK;
}

#ifdef __cplusplus
}
#endif
