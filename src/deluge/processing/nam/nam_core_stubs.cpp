// The subset of NeuralAmpModelerCore we build for the spike avoids get_dsp.cpp and
// nam_file.cpp (they drag in std::filesystem, which we don't want on the device). Two
// spots in the WaveNet sources reference the JSON get_dsp() overload for features the
// baked-in models don't use (condition-DSP submodels and slimmable staging), so satisfy
// the linker with a stub that fails loudly if one is ever reached.

#include "NAM/get_dsp.h"

#include <stdexcept>

namespace nam {

std::unique_ptr<DSP> get_dsp(const nlohmann::json& config, DspLoadOptions options) {
	throw std::runtime_error("nam::get_dsp(json) not supported in Deluge spike build");
}

} // namespace nam
