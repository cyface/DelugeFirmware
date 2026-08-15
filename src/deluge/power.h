/*
 * Copyright (c) 2014-2023 Synthstrom Audible Limited
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
#pragma once
#include <cstdint>

/// Smoothed system-rail reading in millivolts. Updated by inputRoutine() in deluge.cpp.
///
/// This taps the power module's "Voltage Sense" line (SYS_VOLT_SENSE_PIN), which sits downstream of the
/// charger's power path — so it is the *system rail*, not the cell. Running on battery the rail tracks the
/// cell and this is a usable charge signal. On external power it reads the supply rail instead (measured
/// ~4000mV charging a depleted cell, rising past 5000mV as the charge current tapers) and says nothing
/// direct about the cell's state of charge. deluge::power below exists to keep those two cases apart.
extern uint16_t batteryMV;

namespace deluge::power {

enum class State : uint8_t {
	/// Running from the cell. batteryMV tracks cell voltage, so the charge estimate is live.
	OnBattery,
	/// External power connected, charger still delivering appreciable current. The cell is not visible.
	Charging,
	/// External power connected and the charge current has tapered off, so the cell is full.
	Full,
};

/// No charge estimate available: the cell has not been visible since boot (i.e. we booted on external power).
constexpr int32_t kPercentUnknown = -1;

/// Fold a new rail reading into the charge estimate. Called from inputRoutine() every kUpdateIntervalMS.
void update(uint16_t mv);

State state();

/// True whenever the Deluge is running from USB or DC power rather than the cell.
bool onExternalPower();

/// Best available charge estimate, 0-100, or kPercentUnknown.
///
/// Live while OnBattery. While Charging this holds the last on-battery estimate, because the cell simply
/// cannot be measured through the charger — it is real but stale, and minutesOnExternalPower() says how
/// stale. Once the charger signals completion (State::Full) this reads 100 again.
int32_t percent();

/// Whole minutes since external power was connected. Meaningless while OnBattery.
uint32_t minutesOnExternalPower();

/// Map a rail-on-battery reading to a charge percentage via the 18650 discharge curve.
int32_t percentFromMV(uint16_t mv);

} // namespace deluge::power
