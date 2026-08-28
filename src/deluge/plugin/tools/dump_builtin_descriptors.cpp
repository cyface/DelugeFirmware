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
 * Offline tool: print the built-in plugin descriptors as JSON, so pack_dlp.py can turn one into a .dlp without a
 * second, hand-maintained copy of the same names and sizes. Built and run on the development machine (it needs
 * nothing from the firmware but plugin_abi.h and the two builtin_*.h descriptor headers).
 *
 * The only thing here that is not already in a descriptor is the *name* of each entry point - a descriptor holds
 * the function pointer, and the packer needs the ELF symbol to look up in the linked kernel. Each name below is
 * paired with a static_assert that it is the very function the descriptor points at, so a renamed or reordered
 * entry point is a compile error rather than a blob that calls the wrong code.
 *
 *   c++ -std=c++20 -I src/deluge dump_builtin_descriptors.cpp <kernels>.c -o dump_builtin_descriptors
 */
#include "plugin/host/builtin_fx.h"
#include "plugin/host/builtin_sources.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace deluge::plugin;

namespace {

/// An insert FX, with where its kernel lives and what its entry points are called.
struct FxRecord {
	const DelugeFxPlugin* plugin;
	const char* source;        ///< kernel .c, relative to the repository root
	const char* header;        ///< kernel .h, relative to the repository root
	const char* stateSizeExpr; ///< how stateSize is computed, re-evaluated for the target by the packer
	const char* resetSymbol;
	const char* isActiveSymbol; ///< nullptr if the plugin has no isActive
	const char* renderSymbol;
};

/// A source plugin, likewise.
struct SourceRecord {
	const DelugeSourcePlugin* plugin;
	const char* source;
	const char* header;
	const char* voiceStateSizeExpr;
	const char* initSymbol;
	const char* triggerSymbol;
	const char* renderSymbol;
};

static_assert(builtin::kTapeSaturation.reset == tape_saturation_reset);
static_assert(builtin::kTapeSaturation.isActive == tape_saturation_is_active);
static_assert(builtin::kTapeSaturation.render == tape_saturation_render);

constexpr FxRecord kFxRecords[] = {
    {&builtin::kTapeSaturation, "src/deluge/plugin/fx/tape_saturation.c", "src/deluge/plugin/fx/tape_saturation.h",
     "sizeof(TapeSaturationState)", "tape_saturation_reset", "tape_saturation_is_active", "tape_saturation_render"},
};

static_assert(builtin::kPlaitsDrums.init == plaits_drums_init);
static_assert(builtin::kPlaitsDrums.trigger == plaits_drums_trigger);
static_assert(builtin::kPlaitsDrums.render == plaits_drums_render);

constexpr SourceRecord kSourceRecords[] = {
    {&builtin::kPlaitsDrums, "src/deluge/plugin/source/plaits_drums.c", "src/deluge/plugin/source/plaits_drums.h",
     "sizeof(PlaitsDrumsVoice)", "plaits_drums_init", "plaits_drums_trigger", "plaits_drums_render"},
};

void printJsonString(const char* text) {
	if (text == nullptr) {
		printf("null");
		return;
	}
	putchar('"');
	for (const char* c = text; *c != 0; c++) {
		if (*c == '"' || *c == '\\') {
			printf("\\%c", *c);
		}
		else if (static_cast<unsigned char>(*c) < 0x20) {
			printf("\\u%04x", *c);
		}
		else {
			putchar(*c);
		}
	}
	putchar('"');
}

void printField(const char* key, const char* value, const char* trailer = ",\n") {
	printf("    \"%s\": ", key);
	printJsonString(value);
	printf("%s", trailer);
}

void printUint(const char* key, uint32_t value, const char* trailer = ",\n") {
	printf("    \"%s\": %u%s", key, value, trailer);
}

void printName(const DelugePluginName& name) {
	printf("{\"name\": ");
	printJsonString(name.name);
	printf(", \"shortName\": ");
	printJsonString(name.shortName);
	printf("}");
}

void dumpFx(const FxRecord& record) {
	const DelugeFxPlugin& plugin = *record.plugin;
	printf("  {\n");
	printField("kind", "fx");
	printField("name", plugin.name);
	printUint("abiVersion", plugin.abiVersion);
	printField("source", record.source);
	printField("header", record.header);
	printUint("stateSize", plugin.stateSize);
	printField("stateSizeExpr", record.stateSizeExpr);
	printf("    \"entries\": {\"reset\": ");
	printJsonString(record.resetSymbol);
	printf(", \"isActive\": ");
	printJsonString(plugin.isActive != nullptr ? record.isActiveSymbol : nullptr);
	printf(", \"render\": ");
	printJsonString(record.renderSymbol);
	printf("},\n");
	printf("    \"params\": [\n");
	for (uint32_t i = 0; i < plugin.numParams; i++) {
		const DelugeFxParamInfo& param = plugin.paramInfo[i];
		printf("      {\"name\": ");
		printJsonString(param.name);
		printf(", \"shortName\": ");
		printJsonString(param.shortName);
		printf(", \"fileName\": ");
		printJsonString(param.fileName);
		printf(", \"defaultValue\": %d}%s", param.defaultValue, i + 1 < plugin.numParams ? ",\n" : "\n");
	}
	printf("    ]\n  }");
}

void dumpSource(const SourceRecord& record) {
	const DelugeSourcePlugin& plugin = *record.plugin;
	printf("  {\n");
	printField("kind", "source");
	printField("name", plugin.name);
	printUint("abiVersion", plugin.abiVersion);
	printField("source", record.source);
	printField("header", record.header);
	printUint("voiceStateSize", plugin.voiceStateSize);
	printField("voiceStateSizeExpr", record.voiceStateSizeExpr);
	printUint("scratchSize", plugin.scratchSize);
	printUint("maxBlockSize", plugin.maxBlockSize);
	printUint("numMacros", plugin.numMacros);
	printf("    \"entries\": {\"init\": ");
	printJsonString(record.initSymbol);
	printf(", \"trigger\": ");
	printJsonString(record.triggerSymbol);
	printf(", \"render\": ");
	printJsonString(record.renderSymbol);
	printf("},\n");
	printf("    \"models\": [\n");
	for (uint32_t i = 0; i < plugin.numModels; i++) {
		const DelugeSourceModelInfo& model = plugin.modelInfo[i];
		printf("      {\"name\": ");
		printName(model.name);
		printf(", \"fileName\": ");
		printJsonString(model.fileName);
		printf(", \"macros\": [");
		for (uint32_t m = 0; m < DELUGE_SOURCE_PLUGIN_MAX_MACROS; m++) {
			printName(model.macros[m]);
			printf(m + 1 < DELUGE_SOURCE_PLUGIN_MAX_MACROS ? ", " : "");
		}
		printf("]}%s", i + 1 < plugin.numModels ? ",\n" : "\n");
	}
	printf("    ]\n  }");
}

} // namespace

int main() {
	printf("[\n");
	bool first = true;
	for (const FxRecord& record : kFxRecords) {
		printf(first ? "" : ",\n");
		first = false;
		dumpFx(record);
	}
	for (const SourceRecord& record : kSourceRecords) {
		printf(first ? "" : ",\n");
		first = false;
		dumpSource(record);
	}
	printf("\n]\n");
	return 0;
}
