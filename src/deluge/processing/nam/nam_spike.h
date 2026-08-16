#pragma once

// Phase 0 spike: Neural Amp Modeler (NAM) on the Deluge.
//
// Gated behind the "NAM Spike (Dev)" community feature toggle. When enabled:
//  - at boot, a baked-in A2-Lite capture is loaded and the master/headphone output is
//    processed through it (mono-summed). Toggling the community feature off/on at runtime
//    bypasses/re-engages it for A/B listening.
//  - a few seconds after boot, a chunked benchmark measures inference cost
//    (cycles/sample) for A2-Lite (internal + external RAM placement) and a synthetic
//    A1-Nano, then reports via debug log, an on-screen popup, and NAM_BENCH.TXT on SD.
//
// This is investigation code: it deliberately trades polish for measurement.

#include "dsp/stereo_sample.h"
#include <span>

namespace deluge::nam_spike {

// Load the live model and schedule the benchmark. Call once at boot, after runtime
// feature settings have been read. No-op (and no memory used) when the toggle is off.
void init();

// Process the final master mix in place (q31 stereo, engine-internal ~1/256-of-full-scale
// level). Returns immediately when the spike is off or not ready.
void processMasterOutput(std::span<StereoSample> buffer);

} // namespace deluge::nam_spike
