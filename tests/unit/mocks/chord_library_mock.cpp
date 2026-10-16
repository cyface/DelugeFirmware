// Stand-ins for the card-backed half of ChordLibrary (chord_library_file.cpp), which needs FatFs and the runtime
// settings. The unit tests only use the built-in chord sets, so no library file ever loads.
#include "gui/ui/keyboard/chords.h"

namespace deluge::gui::ui::keyboard {

bool ChordLibrary::useFile(char const* name) {
	return false;
}

void ChordLibrary::selectNext() {
}

void ChordLibrary::refreshFromSettings() {
}

const char* ChordLibrary::pageName(int32_t page) const {
	return nullptr;
}

} // namespace deluge::gui::ui::keyboard
