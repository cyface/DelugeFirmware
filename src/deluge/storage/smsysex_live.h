/*
 * Copyright © 2026 Synthstrom Audible Limited
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
 * Live edit over SysEx: lets an external editor treat the instrument the Deluge has loaded as the
 * document it is editing. These are extra ops on the smSysex JSON protocol (see smsysex.cpp), all
 * gated behind the "Sysex Live Edit" community feature and advertised to clients as "live": <version>
 * in the ^session grant.
 *
 *   inst    -> ^inst    what the current clip's instrument is (type, name, folder, edited flag, the
 *                       selected kit row, AFFECT ENTIRE, and a generation counter bumped by every edit)
 *   save    -> ^save    write the current instrument to a preset file, the way SAVE -> SYNTH/KIT does,
 *                       minus the UI: "overwrite" replaces the overwrite menu, "keep" leaves the
 *                       instrument's own name and folder alone (used to pull the live preset via TEMP/)
 *   load    -> ^load    replace the current clip's instrument from a preset file; "name"/"dir" restore
 *                       the instrument's identity afterwards (used to push an edited preset via TEMP/)
 *   select  -> ^select  choose the kit row ("drum") and/or AFFECT ENTIRE ("entire") as if on the pads
 *   param   -> ^param   set or read one automatable parameter by its file name, as a full int32
 *   sub     -> ^sub     lease this cable for pushes ("secs"; 0 releases). While the lease holds the Deluge
 *                       sends, unrequested (sequence number 0, command Json):
 *                         ^chg    parameter values that changed on the device, batched every 25 ms
 *                         ^dirty  something other than a parameter value changed: pull the preset
 *                         ^inst   the instrument, selected row, AFFECT ENTIRE or edited flag changed
 */

#pragma once

#include "definitions_cxx.hpp"

class Instrument;
class JsonDeserializer;
class MIDICable;
class ModelStackWithAutoParam;
class String;

namespace smSysex::live {

/// Protocol version advertised in the ^session grant while the feature is on.
constexpr int32_t kProtocolVersion = 1;

/// Whether the community feature toggle is on. Every op replies err/why "off" when it is not.
bool enabled();

/// Generation counter: bumped on every edit to the current instrument, reported by ^inst.
uint32_t generation();
void bumpGeneration();

void getInstrument(MIDICable& cable, JsonDeserializer& reader);
void savePreset(MIDICable& cable, JsonDeserializer& reader);
void loadPreset(MIDICable& cable, JsonDeserializer& reader);
void selectRow(MIDICable& cable, JsonDeserializer& reader);
void setParam(MIDICable& cable, JsonDeserializer& reader);
void subscribe(MIDICable& cable, JsonDeserializer& reader);

/// Hook: a parameter's current value changed (ParamManager::notifyParamModifiedInSomeWay). Cheap when
/// nobody is subscribed; never sends anything itself.
void noteParamChanged(ModelStackWithAutoParam const* modelStack);
/// Hook: an instrument was edited in some way (Instrument::beenEdited).
void noteEdited(Instrument* instrument);
/// Called on every pass of the SysEx task: drains the pending changes to the subscriber.
void tick();

/// Load the preset at `path` and swap it in for `oldInstrument` everywhere the song uses it. The song's
/// hibernating copy with the same slot is dropped first so the file is what gets read. `name`/`dirPath`
/// are what the new instrument is registered as (usually the file's own). Shared with the close-triggered
/// hot reload in smsysex.cpp.
Error swapInstrumentFromFile(Instrument* oldInstrument, char const* path, String* name, String* dirPath,
                             Instrument** newInstrumentOut);

} // namespace smSysex::live
