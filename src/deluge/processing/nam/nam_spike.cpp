// NAM core (and the std headers it drags in) must come before any firmware header:
// io/debug/log.h pulls in lib/printf.h, whose snprintf-renaming macros break <string> and
// friends if those get included afterwards.
#include "NAM/activations.h"
#include "NAM/dsp.h"
#include "NAM/wavenet/model.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "processing/nam/nam_spike.h"

#include "OSLikeStuff/scheduler_api.h"
#include "definitions_cxx.hpp"
#include "fatfs/ff.h"
#include "hid/display/display.h"
#include "io/debug/log.h"
#include "memory/memory_placement.h"
#include "model/settings/runtime_feature_settings.h"
#include "processing/nam/nam_baked_models.h"

namespace deluge::nam_spike {

namespace {

constexpr double kCpuHz = 400000000.0;

// Engine-internal signal sits ~48dB (2^8) below q31 full scale, so full scale is 2^23.
constexpr float kToFloat = 1.0f / 8388608.0f;
constexpr float kFromFloat = 8388608.0f;

std::unique_ptr<nam::DSP> liveModel;
bool liveReady = false;

std::unique_ptr<nam::DSP> buildModel(const char* configJson, const float* weights, int numWeights,
                                     bool preferInternalRam) {
	deluge::memory::preferInternalNextAllocs = preferInternalRam;
	std::unique_ptr<nam::DSP> dsp;
	try {
		nlohmann::json config = nlohmann::json::parse(configJson);
		auto modelConfig = nam::wavenet::create_config(config, kSampleRate);
		std::vector<float> weightVector(weights, weights + numWeights);
		dsp = modelConfig->create(std::move(weightVector), kSampleRate);
		dsp->ResetAndPrewarm(kSampleRate, SSI_TX_BUFFER_NUM_SAMPLES);
	} catch (...) {
		dsp.reset();
	}
	deluge::memory::preferInternalNextAllocs = false;
	return dsp;
}

// ------------------------------------------------------------------ benchmark ----

struct BenchCase {
	const char* name;
	const char* configJson;
	const float* weights;
	int numWeights;
	bool internalRam;
	double seconds;
	int samplesTimed;
};

BenchCase benchCases[] = {
    {"A2-Lite int", kNamA2LiteConfigJson, kNamA2LiteWeights, kNamA2LiteNumWeights, true, 0.0, 0},
    {"A2-Lite ext", kNamA2LiteConfigJson, kNamA2LiteWeights, kNamA2LiteNumWeights, false, 0.0, 0},
    {"A1-Nano int", kNamA1NanoConfigJson, kNamA1NanoWeights, kNamA1NanoNumWeights, true, 0.0, 0},
};
constexpr int kNumBenchCases = sizeof(benchCases) / sizeof(benchCases[0]);

constexpr int kBenchBlock = 64;    // samples per process() call, like a typical render window
constexpr int kBenchWarmup = 1024; // untimed samples after model build
constexpr int kBenchTimed = 16384; // timed samples per case

int benchCase = 0;
int benchSamplesDone = 0; // includes warmup
std::unique_ptr<nam::DSP> benchModel;
bool benchModelFailed = false;
char benchReport[256];

uint32_t noiseSeed = 12345;

void fillNoise(float* buffer, int num) {
	for (int i = 0; i < num; i++) {
		noiseSeed = noiseSeed * 1664525u + 1013904223u;
		buffer[i] = static_cast<int32_t>(noiseSeed) * (0.25f / 2147483648.0f);
	}
}

void writeBenchReportToSD() {
	FIL file;
	if (f_open(&file, "NAM_BENCH.TXT", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
		D_PRINTLN("NAM bench: SD write failed");
		return;
	}
	UINT written;
	f_write(&file, benchReport, strlen(benchReport), &written);
	f_close(&file);
	D_PRINTLN("NAM bench: wrote NAM_BENCH.TXT");
}

void finishBenchmark() {
	char* p = benchReport;
	char* end = benchReport + sizeof(benchReport);
	p += snprintf(p, end - p, "NAM benchmark (44.1kHz, block %d):\n", kBenchBlock);
	for (auto& c : benchCases) {
		if (c.samplesTimed == 0) {
			p += snprintf(p, end - p, "%s: FAILED to build\n", c.name);
			continue;
		}
		double cyclesPerSample = c.seconds * kCpuHz / c.samplesTimed;
		double percentCpu = c.seconds * kSampleRate / c.samplesTimed * 100.0;
		p += snprintf(p, end - p, "%s: %d cy/sample, %d%% CPU\n", c.name, (int)cyclesPerSample, (int)percentCpu);
	}
	D_PRINTLN("%s", benchReport);

	// Headline popup: the first case that produced a number
	for (auto& c : benchCases) {
		if (c.samplesTimed != 0) {
			char popup[32];
			double percentCpu = c.seconds * kSampleRate / c.samplesTimed * 100.0;
			snprintf(popup, sizeof(popup), "NAM %d%%", (int)percentCpu);
			display->displayPopup(popup);
			break;
		}
	}
	addOnceTask(writeBenchReportToSD, 200, 0.5, "nam bench sd", RESOURCE_SD);
}

void benchTask() {
	if (benchCase >= kNumBenchCases) {
		finishBenchmark();
		return;
	}
	BenchCase& c = benchCases[benchCase];

	if (benchModel == nullptr && !benchModelFailed) {
		benchModel = buildModel(c.configJson, c.weights, c.numWeights, c.internalRam);
		benchModelFailed = (benchModel == nullptr);
		if (benchModelFailed) {
			D_PRINTLN("NAM bench: %s failed to build", c.name);
		}
	}

	if (benchModelFailed) {
		benchCase++;
		benchModelFailed = false;
		benchSamplesDone = 0;
	}
	else {
		// One short chunk per task invocation, so audio keeps running in between
		static float in[kBenchBlock];
		static float out[kBenchBlock];
		float* inPtr = in;
		float* outPtr = out;
		for (int block = 0; block < 2; block++) {
			fillNoise(in, kBenchBlock);
			double t0 = getSystemTime();
			benchModel->process(&inPtr, &outPtr, kBenchBlock);
			double t1 = getSystemTime();
			benchSamplesDone += kBenchBlock;
			if (benchSamplesDone > kBenchWarmup) {
				c.seconds += t1 - t0;
				c.samplesTimed += kBenchBlock;
			}
		}
		if (benchSamplesDone >= kBenchWarmup + kBenchTimed) {
			benchModel.reset();
			benchCase++;
			benchSamplesDone = 0;
		}
	}
	addOnceTask(benchTask, 200, 0.008, "nam bench", RESOURCE_NONE);
}

} // namespace

// ------------------------------------------------------------------ public API ----

void init() {
	if (!runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::NamSpike)) {
		return;
	}
	nam::activations::Activation::enable_fast_tanh();
	liveModel = buildModel(kNamA2LiteConfigJson, kNamA2LiteWeights, kNamA2LiteNumWeights, true);
	liveReady = (liveModel != nullptr);
	if (!liveReady) {
		display->displayPopup("NAM load fail");
		D_PRINTLN("NAM spike: live model failed to load");
		return;
	}
	D_PRINTLN("NAM spike: live A2-Lite model loaded, benchmark scheduled");
	addOnceTask(benchTask, 200, 4.0, "nam bench", RESOURCE_NONE);
}

void processMasterOutput(std::span<StereoSample> buffer) {
	if (!liveReady || !runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::NamSpike)) {
		return;
	}
	static float in[SSI_TX_BUFFER_NUM_SAMPLES];
	static float out[SSI_TX_BUFFER_NUM_SAMPLES];

	size_t num = std::min(buffer.size(), (size_t)SSI_TX_BUFFER_NUM_SAMPLES);
	for (size_t i = 0; i < num; i++) {
		// Mono sum at half gain, scaled to nominal +/-1.0 full scale for the model
		int32_t mono = (buffer[i].l >> 1) + (buffer[i].r >> 1);
		in[i] = (float)mono * kToFloat;
	}
	float* inPtr = in;
	float* outPtr = out;
	liveModel->process(&inPtr, &outPtr, (int)num);
	for (size_t i = 0; i < num; i++) {
		float y = std::clamp(out[i], -128.0f, 128.0f) * kFromFloat;
		int32_t sample = (int32_t)y;
		buffer[i].l = sample;
		buffer[i].r = sample;
	}
}

} // namespace deluge::nam_spike
