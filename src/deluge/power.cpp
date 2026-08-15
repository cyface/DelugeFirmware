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
#include "power.h"
#include <array>
#include <cstddef>

namespace deluge::power {

namespace {

/// inputRoutine() re-arms READ_INPUTS at 100ms, so update() runs at roughly this rate.
constexpr uint32_t kUpdateIntervalMS = 100;
constexpr uint32_t kUpdatesPerSecond = 1000 / kUpdateIntervalMS;
constexpr uint32_t kUpdatesPerMinute = 60 * kUpdatesPerSecond;

struct CurvePoint {
	uint16_t mv;
	uint8_t percent;
};

/// Discharge curve for the stock 18650, as seen at the rail with the Deluge running.
///
/// The previous estimate mapped 2600mV-4200mV linearly, which put 0% below the cell's own protection cutoff
/// and so made the bottom third of the scale unreachable: a cell flat enough to trigger the low-battery LED
/// (3200mV, see inputRoutine()) still displayed 37%, and the critical blinking state (2900mV) displayed 19%.
/// A Li-ion discharge curve is far from linear — it is very flat between roughly 3.6V and 3.9V, where most of
/// the usable capacity sits, then falls off a cliff — so interpolate real curve points instead. The bottom of
/// this table is anchored to the thresholds inputRoutine() already uses to drive the battery LED, so the
/// percentage and the LED now agree about when the battery is nearly gone.
///
/// These are loaded readings, not resting cell voltage: the rail sags under the Deluge's own draw, so the
/// figures sit a little below a datasheet open-circuit curve. Expect a few percent of wobble as the audio
/// engine's load varies.
constexpr std::array<CurvePoint, 13> kDischargeCurve{{
    {4200, 100},
    {4060, 90},
    {3950, 80},
    {3870, 70},
    {3800, 60},
    {3740, 50},
    {3690, 40},
    {3640, 30},
    {3580, 20},
    {3480, 10},
    {3380, 5},
    {3200, 2}, // Low-battery LED goes solid here.
    {2900, 0}, // Low-battery LED starts blinking here; protection circuit cuts out not far below.
}};

/// A single 18650 cannot exceed this, so anything above it has to be an external supply.
constexpr uint16_t kCellMaxMV = 4200;
constexpr uint16_t kExternalCertainMV = 4300;

/// Rail step over kHistorySeconds that means a supply was plugged in or pulled out. Connecting power lifts the
/// rail by hundreds of mV within a second or two; the cell itself cannot move anywhere near that fast, and load
/// transients from the audio engine are worth well under 250mV, so this separates the two cleanly.
constexpr uint16_t kStepMV = 250;
constexpr std::size_t kHistorySeconds = 4;

/// Rail voltage at which we call the charge complete.
///
/// The rail reads low while the charger is pulling hard (~4000mV observed into a depleted cell) and climbs as
/// the current tapers, because less current means less drop across the supply and cable. So a high rail means
/// the charger has essentially stopped. PROVISIONAL: the ceiling depends on the supply and cable in use, and
/// this is calibrated against a USB source that settles slightly above 5000mV. The menu always shows raw mV
/// alongside the estimate so this can be re-tuned against real hardware.
constexpr uint16_t kChargeCompleteMV = 4900;
constexpr uint16_t kChargeCompleteHysteresisMV = 100;
/// Require the rail to stay up this long before believing the charge finished, so a transient cannot trip it.
constexpr uint32_t kChargeCompleteHoldUpdates = 10 * kUpdatesPerSecond;

State currentState = State::OnBattery;
int32_t lastKnownPercent = kPercentUnknown;
uint32_t updatesOnExternalPower = 0;
uint32_t chargeCompleteHoldUpdates = 0;

uint16_t history[kHistorySeconds] = {};
std::size_t historyIndex = 0;
uint32_t updatesSinceHistoryPush = 0;
bool primed = false;

} // namespace

int32_t percentFromMV(uint16_t mv) {
	if (mv >= kDischargeCurve.front().mv) {
		return 100;
	}
	if (mv <= kDischargeCurve.back().mv) {
		return 0;
	}

	for (std::size_t i = 1; i < kDischargeCurve.size(); i++) {
		const CurvePoint& lower = kDischargeCurve[i];
		if (mv >= lower.mv) {
			const CurvePoint& upper = kDischargeCurve[i - 1];
			const int32_t spanMV = upper.mv - lower.mv;
			const int32_t spanPercent = upper.percent - lower.percent;
			return lower.percent + (((int32_t)(mv - lower.mv) * spanPercent + (spanMV >> 1)) / spanMV);
		}
	}
	return 0;
}

void update(uint16_t mv) {
	if (!primed) {
		primed = true;
		for (uint16_t& sample : history) {
			sample = mv;
		}
		// Booting with a supply already connected is the one case where we never get to see the cell.
		currentState = (mv > kExternalCertainMV) ? State::Charging : State::OnBattery;
	}

	// The slot due to be overwritten next is the oldest reading we still hold.
	const uint16_t oldest = history[historyIndex];
	const bool rose = (mv > oldest) && ((mv - oldest) > kStepMV);
	const bool fell = (oldest > mv) && ((oldest - mv) > kStepMV);

	if (currentState == State::OnBattery) {
		if (mv > kExternalCertainMV || rose) {
			currentState = State::Charging;
			updatesOnExternalPower = 0;
			chargeCompleteHoldUpdates = 0;
		}
	}
	else if (fell && mv <= kCellMaxMV) {
		currentState = State::OnBattery;
	}

	if (currentState == State::OnBattery) {
		lastKnownPercent = percentFromMV(mv);
		updatesOnExternalPower = 0;
	}
	else {
		updatesOnExternalPower++;

		if (mv >= kChargeCompleteMV) {
			if (chargeCompleteHoldUpdates < kChargeCompleteHoldUpdates) {
				chargeCompleteHoldUpdates++;
			}
			else {
				currentState = State::Full;
			}
		}
		else if (mv < (kChargeCompleteMV - kChargeCompleteHysteresisMV)) {
			chargeCompleteHoldUpdates = 0;
			currentState = State::Charging;
		}
	}

	if (++updatesSinceHistoryPush >= kUpdatesPerSecond) {
		updatesSinceHistoryPush = 0;
		history[historyIndex] = mv;
		historyIndex = (historyIndex + 1) % kHistorySeconds;
	}
}

State state() {
	return currentState;
}

bool onExternalPower() {
	return currentState != State::OnBattery;
}

int32_t percent() {
	// A completed charge is the one thing external power does tell us about the cell.
	if (currentState == State::Full) {
		return 100;
	}
	return lastKnownPercent;
}

uint32_t minutesOnExternalPower() {
	return updatesOnExternalPower / kUpdatesPerMinute;
}

} // namespace deluge::power
