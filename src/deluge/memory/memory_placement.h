#pragma once

namespace deluge::memory {

// When true, global operator new tries internal RAM first (via allocMaxSpeed) instead of
// going straight to external SDRAM. Used by the NAM spike to compare inference speed with
// model state in internal vs external RAM. Not interrupt-safe: set/clear around a
// construction site only, from task context.
extern bool preferInternalNextAllocs;

} // namespace deluge::memory
