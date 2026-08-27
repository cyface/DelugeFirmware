#include "io/debug/plugin_spike.h"
#include "definitions.h"
#include "fatfs/ff.h"
#include "io/debug/print.h"
#include "io/midi/sysex.h"
#include <algorithm>
#include <cstdint>
#include <cstring>

extern "C" {
#include "RZA1/cache/cache.h"
#include "util/cfunctions.h"
// The reference: the exact kernel source the blob is built from, compiled into internal .text.
#include "../../../../contrib/plugin_spike/spike_blob.h"
#include "../../../../contrib/plugin_spike/spike_plugin.c"
}

namespace Debug {
extern MIDICable* midiDebugCable;
}

namespace {

constexpr uint32_t kBlock = 128;
constexpr uint32_t kTrials = 9;
constexpr char const* kPluginPath = "PLUGINS/spike.dlp";

// Plugin homes in SDRAM. The blob is copied in at runtime, so these are plain buffers.
PLACE_SDRAM_BSS alignas(32) uint8_t sdramEmbedded[1024];
PLACE_SDRAM_BSS alignas(32) uint8_t sdramFile[8192];
PLACE_SDRAM_BSS alignas(32) int32_t outBuf[kBlock];

uint32_t volatile hostCalls;
uint32_t volatile sink;

[[gnu::noinline]] int32_t hostMulQ31(int32_t a, int32_t b) {
	hostCalls = hostCalls + 1;
	return (int32_t)(((int64_t)a * b) >> 31);
}

SpikeHostApi const hostApi = {hostMulQ31};
SpikeParams const params = {0x01234567u, 0x00FEDCBAu, 0x20000000, 0x60000000};

// Make bytes just written by the CPU visible to instruction fetch at the same address:
// clean D + L2 (so RAM holds them), invalidate I + branch predictor (so nothing stale is served).
void makeCodeVisible(void* start, uint32_t len) {
	invalidate_range_all_caches((uintptr_t)start, (uintptr_t)start + len);
	L1_I_CacheFlushAll();
	__asm__ volatile("mcr p15, 0, %0, c7, c5, 6" ::"r"(0)); // BPIALL
	__asm__ volatile("dsb\nisb");
}

void makeAllCachesCold() {
	L1_D_CacheWritebackFlushAll();
	L2CacheCleanInvalidateAll();
	L1_I_CacheFlushAll();
	__asm__ volatile("mcr p15, 0, %0, c7, c5, 6" ::"r"(0)); // BPIALL
	__asm__ volatile("dsb\nisb");
}

struct Stats {
	uint32_t min;
	uint32_t median;
	uint32_t check;
	uint32_t calls;
};

Stats measure(SpikeRenderFn fn, bool cold) {
	uint32_t samples[kTrials];
	uint32_t check = 0;
	uint32_t calls = 0;
	SpikeState state;
	if (!cold) {
		state = SpikeState{};
		sink = fn(&hostApi, &state, outBuf, kBlock, &params); // prime
	}
	for (uint32_t t = 0; t < kTrials; t++) {
		state = SpikeState{};
		if (cold) {
			makeAllCachesCold();
		}
		uint32_t callsBefore = hostCalls;
		uint32_t t0 = Debug::readCycleCounter();
		uint32_t r = fn(&hostApi, &state, outBuf, kBlock, &params);
		uint32_t t1 = Debug::readCycleCounter();
		check = r;
		calls = hostCalls - callsBefore;
		samples[t] = t1 - t0;
	}
	std::sort(samples, samples + kTrials);
	return Stats{samples[0], samples[kTrials / 2], check, calls};
}

// --- report plumbing (same constraints as sdram_text_bench: release builds have no Debug::print,
// sysexDebugPrint shares one buffer, so queue lines and emit one per task tick) ---
char lineBuf[256];
uint32_t linePos;
constexpr uint32_t kMaxReportLines = 12;
char reportLines[kMaxReportLines][sizeof(lineBuf)];
uint32_t numReportLines;
uint32_t nextReportLine;

void lineStart() {
	linePos = 0;
	lineBuf[0] = 0;
}
void append(char const* s) {
	while (*s != 0 && linePos < sizeof(lineBuf) - 1) {
		lineBuf[linePos++] = *s++;
	}
	lineBuf[linePos] = 0;
}
void appendNum(uint32_t value) {
	char buf[12];
	intToString((int32_t)value, buf, 1);
	append(buf);
}
void appendHex(uint32_t value) {
	static char const digits[] = "0123456789ABCDEF";
	char buf[11] = "0x";
	for (int32_t i = 0; i < 8; i++) {
		buf[2 + i] = digits[(value >> (28 - i * 4)) & 0xF];
	}
	buf[10] = 0;
	append(buf);
}
void lineQueue() {
	if (numReportLines < kMaxReportLines) {
		memcpy(reportLines[numReportLines++], lineBuf, linePos + 1);
	}
}
void kv(char const* key, uint32_t value, bool hex = false) {
	append(",\"");
	append(key);
	append("\":");
	if (hex) {
		append("\"");
		appendHex(value);
		append("\"");
	}
	else {
		appendNum(value);
	}
}

void reportVariant(char const* name, uint32_t addr, Stats cold, Stats warm) {
	lineStart();
	append("{\"spike\":\"plugin\",\"variant\":\"");
	append(name);
	append("\"");
	kv("addr", addr, true);
	kv("cold_min", cold.min);
	kv("cold_med", cold.median);
	kv("warm_min", warm.min);
	kv("warm_med", warm.median);
	kv("check", warm.check, true);
	kv("host_calls", warm.calls);
	append("}");
	lineQueue();
}

// Validate a blob sitting in memory and return its entry as a callable (Thumb bit set), or nullptr.
SpikeRenderFn entryOf(uint8_t const* base, uint32_t size, char const* what) {
	DlpHeader hdr;
	memcpy(&hdr, base, sizeof(hdr));
	bool ok = size >= sizeof(hdr) && hdr.magic == DLP_MAGIC && hdr.abi == DLP_ABI && hdr.entryOffset < size;
	lineStart();
	append("{\"spike\":\"plugin\",\"info\":\"header\",\"source\":\"");
	append(what);
	append("\"");
	kv("base", (uint32_t)base, true);
	kv("size", size);
	kv("magic", hdr.magic, true);
	kv("abi", hdr.abi);
	kv("entry", hdr.entryOffset);
	kv("ok", ok ? 1 : 0);
	append("}");
	lineQueue();
	if (!ok) {
		return nullptr;
	}
	return (SpikeRenderFn)((uintptr_t)base + hdr.entryOffset + 1); // +1: Thumb entry
}

bool requested = false;
bool hasRun = false;

void runSpike() {
	Debug::init(); // idempotent; enables the PMU cycle counter

	// 1. Reference: the same kernel linked into internal .text.
	SpikeRenderFn internalFn = &spike_render;
	Stats refCold = measure(internalFn, true);
	Stats refWarm = measure(internalFn, false);
	reportVariant("internal", (uint32_t)internalFn, refCold, refWarm);

	// 2. Embedded blob copied into SDRAM at runtime.
	memcpy(sdramEmbedded, kSpikeBlob, kSpikeBlobSize);
	makeCodeVisible(sdramEmbedded, kSpikeBlobSize);
	SpikeRenderFn embFn = entryOf(sdramEmbedded, kSpikeBlobSize, "embedded");
	if (embFn != nullptr) {
		Stats c = measure(embFn, true);
		Stats w = measure(embFn, false);
		reportVariant("sdram_embedded", (uint32_t)embFn, c, w);
	}

	// 3. The real thing: PLUGINS/spike.dlp read from the card into SDRAM.
	FIL file;
	FRESULT err = f_open(&file, kPluginPath, FA_READ);
	if (err != FR_OK) {
		lineStart();
		append("{\"spike\":\"plugin\",\"info\":\"file\",\"path\":\"");
		append(kPluginPath);
		append("\"");
		kv("f_open", (uint32_t)err);
		append("}");
		lineQueue();
	}
	else {
		UINT got = 0;
		err = f_read(&file, sdramFile, sizeof(sdramFile), &got);
		f_close(&file);
		lineStart();
		append("{\"spike\":\"plugin\",\"info\":\"file\",\"path\":\"");
		append(kPluginPath);
		append("\"");
		kv("f_read", (uint32_t)err);
		kv("bytes", got);
		append("}");
		lineQueue();
		if (err == FR_OK && got > 0) {
			makeCodeVisible(sdramFile, got);
			SpikeRenderFn fileFn = entryOf(sdramFile, got, "file");
			if (fileFn != nullptr) {
				Stats c = measure(fileFn, true);
				Stats w = measure(fileFn, false);
				reportVariant("sdram_file", (uint32_t)fileFn, c, w);
				lineStart();
				append("{\"spike\":\"plugin\",\"info\":\"verdict\"");
				kv("bit_exact", (w.check == refWarm.check && (embFn == nullptr || w.check == refWarm.check)) ? 1 : 0);
				kv("ref_check", refWarm.check, true);
				kv("file_check", w.check, true);
				append("}");
				lineQueue();
			}
		}
	}
	lineStart();
	append("{\"spike\":\"plugin\",\"info\":\"done\"}");
	lineQueue();
}

} // namespace

void pluginSpikeRequest() {
	// Re-run on every console attach so a new PLUGINS/spike.dlp can be tested without a reboot.
	requested = true;
	hasRun = false;
	numReportLines = 0;
	nextReportLine = 0;
}

void pluginSpikeRoutine() {
	if (!requested || Debug::midiDebugCable == nullptr) {
		return;
	}
	if (hasRun) {
		if (nextReportLine < numReportLines) {
			Debug::sysexDebugPrint(*Debug::midiDebugCable, reportLines[nextReportLine++], true);
		}
		return;
	}
	hasRun = true;
	runSpike();
}
