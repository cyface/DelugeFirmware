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
 * Offline tool: read a .dlp back through plugin_blob.h - the same validate + bind code the firmware loader will
 * run - and print the resulting descriptor as JSON. pack_dlp.py diffs that against the descriptor it packed from,
 * so every field makes the round trip through the file format before any of this reaches hardware.
 *
 * The image is not copied anywhere here: it binds with the image in place inside the file buffer, and prints
 * entry points as offsets from it (never calling them - this is x86/arm64, the code inside is Thumb-2).
 *
 *   cc -std=gnu23 -I src/deluge read_dlp.c -o read_dlp && ./read_dlp plugin.dlp
 */
#include "plugin/plugin_blob.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const char* statusText(DelugePluginBlobStatus status) {
	switch (status) {
	case DELUGE_PLUGIN_BLOB_OK:
		return "ok";
	case DELUGE_PLUGIN_BLOB_BAD_MAGIC:
		return "not a .dlp (bad magic)";
	case DELUGE_PLUGIN_BLOB_BAD_FORMAT:
		return "unknown container format version";
	case DELUGE_PLUGIN_BLOB_BAD_ABI:
		return "built against a different plugin ABI version";
	case DELUGE_PLUGIN_BLOB_BAD_SIZE:
		return "truncated or wrong fileSize";
	case DELUGE_PLUGIN_BLOB_BAD_KIND:
		return "unknown plugin kind";
	case DELUGE_PLUGIN_BLOB_BAD_OFFSET:
		return "an offset lies outside the file";
	case DELUGE_PLUGIN_BLOB_BAD_ENTRY:
		return "a missing or out-of-range entry point";
	case DELUGE_PLUGIN_BLOB_BAD_CRC:
		return "CRC mismatch";
	case DELUGE_PLUGIN_BLOB_TOO_MANY:
		return "more params or models than storage holds";
	}
	return "unknown status";
}

static void printJsonString(const char* text) {
	if (text == NULL) {
		printf("null");
		return;
	}
	putchar('"');
	for (const char* c = text; *c != 0; c++) {
		if (*c == '"' || *c == '\\') {
			printf("\\%c", *c);
		}
		else if ((unsigned char)*c < 0x20) {
			printf("\\u%04x", *c);
		}
		else {
			putchar(*c);
		}
	}
	putchar('"');
}

/// An entry point as its offset into the image, i.e. what the file stored, recovered from the bound pointer.
static void printEntry(const char* key, const void* fn, const void* imageBase, const char* trailer) {
	if (fn == NULL) {
		printf("\"%s\": null%s", key, trailer);
	}
	else {
		printf("\"%s\": %u%s", key, (unsigned)((uintptr_t)fn - (uintptr_t)imageBase), trailer);
	}
}

static void printName(const DelugePluginName* name) {
	printf("{\"name\": ");
	printJsonString(name->name);
	printf(", \"shortName\": ");
	printJsonString(name->shortName);
	printf("}");
}

int main(int argc, char** argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: read_dlp <plugin.dlp>\n");
		return 2;
	}
	FILE* file = fopen(argv[1], "rb");
	if (file == NULL) {
		fprintf(stderr, "read_dlp: cannot open %s\n", argv[1]);
		return 2;
	}
	fseek(file, 0, SEEK_END);
	long fileSize = ftell(file);
	fseek(file, 0, SEEK_SET);
	uint8_t* data = malloc((size_t)fileSize);
	if (data == NULL || fread(data, 1, (size_t)fileSize, file) != (size_t)fileSize) {
		fprintf(stderr, "read_dlp: cannot read %s\n", argv[1]);
		return 2;
	}
	fclose(file);

	DelugePluginBlobStatus status = deluge_plugin_blob_validate(data, (uint32_t)fileSize);
	if (status != DELUGE_PLUGIN_BLOB_OK) {
		fprintf(stderr, "read_dlp: %s rejected: %s\n", argv[1], statusText(status));
		return 1;
	}
	const DelugePluginBlobHeader* header = (const DelugePluginBlobHeader*)data;
	/* Where the loader would have copied the image; here it stays in the file buffer, unexecuted. */
	const void* imageBase = data + header->imageOffset;

	printf("{\n");
	if (header->kind == DELUGE_PLUGIN_BLOB_KIND_FX) {
		DelugeFxPlugin plugin;
		DelugeFxParamInfo params[64];
		status = deluge_plugin_blob_bind_fx(data, imageBase, &plugin, params,
		                                    (uint32_t)(sizeof(params) / sizeof(params[0])));
		if (status != DELUGE_PLUGIN_BLOB_OK) {
			fprintf(stderr, "read_dlp: bind failed: %s\n", statusText(status));
			return 1;
		}
		printf("  \"kind\": \"fx\",\n  \"name\": ");
		printJsonString(plugin.name);
		printf(",\n  \"abiVersion\": %u,\n", plugin.abiVersion);
		printf("  \"imageSize\": %u,\n", header->imageSize);
		printf("  \"stateSize\": %u,\n", plugin.stateSize);
		printf("  \"entries\": {");
		printEntry("reset", (const void*)plugin.reset, imageBase, ", ");
		printEntry("isActive", (const void*)plugin.isActive, imageBase, ", ");
		printEntry("render", (const void*)plugin.render, imageBase, "},\n");
		printf("  \"params\": [\n");
		for (uint32_t i = 0; i < plugin.numParams; i++) {
			printf("    {\"name\": ");
			printJsonString(plugin.paramInfo[i].name);
			printf(", \"shortName\": ");
			printJsonString(plugin.paramInfo[i].shortName);
			printf(", \"fileName\": ");
			printJsonString(plugin.paramInfo[i].fileName);
			printf(", \"defaultValue\": %d}%s", plugin.paramInfo[i].defaultValue,
			       i + 1 < plugin.numParams ? ",\n" : "\n");
		}
		printf("  ]\n");
	}
	else {
		DelugeSourcePlugin plugin;
		DelugeSourceModelInfo models[64];
		status = deluge_plugin_blob_bind_source(data, imageBase, &plugin, models,
		                                        (uint32_t)(sizeof(models) / sizeof(models[0])));
		if (status != DELUGE_PLUGIN_BLOB_OK) {
			fprintf(stderr, "read_dlp: bind failed: %s\n", statusText(status));
			return 1;
		}
		printf("  \"kind\": \"source\",\n  \"name\": ");
		printJsonString(plugin.name);
		printf(",\n  \"abiVersion\": %u,\n", plugin.abiVersion);
		printf("  \"imageSize\": %u,\n", header->imageSize);
		printf("  \"voiceStateSize\": %u,\n", plugin.voiceStateSize);
		printf("  \"scratchSize\": %u,\n", plugin.scratchSize);
		printf("  \"maxBlockSize\": %u,\n", plugin.maxBlockSize);
		printf("  \"numMacros\": %u,\n", plugin.numMacros);
		printf("  \"entries\": {");
		printEntry("init", (const void*)plugin.init, imageBase, ", ");
		printEntry("trigger", (const void*)plugin.trigger, imageBase, ", ");
		printEntry("render", (const void*)plugin.render, imageBase, "},\n");
		printf("  \"models\": [\n");
		for (uint32_t i = 0; i < plugin.numModels; i++) {
			printf("    {\"name\": ");
			printName(&plugin.modelInfo[i].name);
			printf(", \"fileName\": ");
			printJsonString(plugin.modelInfo[i].fileName);
			printf(", \"macros\": [");
			for (uint32_t m = 0; m < DELUGE_SOURCE_PLUGIN_MAX_MACROS; m++) {
				printName(&plugin.modelInfo[i].macros[m]);
				printf(m + 1 < DELUGE_SOURCE_PLUGIN_MAX_MACROS ? ", " : "");
			}
			printf("]}%s", i + 1 < plugin.numModels ? ",\n" : "\n");
		}
		printf("  ]\n");
	}
	printf("}\n");
	free(data);
	return 0;
}
