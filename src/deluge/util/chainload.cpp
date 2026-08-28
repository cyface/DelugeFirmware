/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute
 * it and/or modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "chainload.h"
#include "RZA1/cache/cache.h"
#include "RZA1/mtu/mtu.h"
#include "definitions.h"
#include "timers_interrupts/timers_interrupts.h"

extern uint32_t spareRenderingBuffer[][SSI_TX_BUFFER_NUM_SAMPLES];
#if defined(__arm__)
extern "C" void v7_dma_flush_range(uint32_t start, uint32_t end);
#endif
extern "C" {
#include "drivers/dmac/dmac.h"
#include "util/cfunctions.h"
}

static bool dmaChannelActive(int32_t channel) {
	return (DMACn(channel).CHSTAT_n & DMAC0_CHSTAT_n_EN) != 0;
}

void chainload_from_buf(uint8_t* buffer, int buf_size) {
	uint32_t user_code_start = *(uint32_t*)(buffer + OFF_USER_CODE_START);
	uint32_t user_code_end = *(uint32_t*)(buffer + OFF_USER_CODE_END);
	uint32_t user_code_exec = *(uint32_t*)(buffer + OFF_USER_CODE_EXECUTE);

	int32_t code_size = (int32_t)(user_code_end - user_code_start);
	if (code_size > buf_size) {
		FREEZE_WITH_ERROR("E994");
		return;
	}

	// Stop the timers so nothing queues another OLED frame or PIC message, then let any DMA still
	// running on the OLED SPI and PIC UART channels finish. Disabling interrupts does not stop a DMA,
	// and the new image re-initialises those channels early in its boot; doing that to a channel that
	// is mid-transfer leaves the PIC link or the display wedged (seen as "booted, answers sysex, but
	// pads and screen are dead"). The pad progress bar and the screensaver keep both channels busy
	// right up to the load message, so this window is real. TIMER_SYSTEM_SLOW stays on for delayMS().
	disableTimer(TIMER_MIDI_GATE_OUTPUT);
	disableTimer(TIMER_SYSTEM_FAST);
	disableTimer(TIMER_SYSTEM_SUPERFAST);
	for (int32_t i = 0; i < 50 && (dmaChannelActive(OLED_SPI_DMA_CHANNEL) || dmaChannelActive(PIC_TX_DMA_CHANNEL));
	     i++) {
		delayMS(1);
	}
	disableTimer(TIMER_SYSTEM_SLOW);

	// Disable interrupts so we don't get interrupted during the chainload
#if defined(__arm__)
	ENTER_CRITICAL_SECTION();
#endif
	uint8_t* funcbuf = reinterpret_cast<uint8_t*>(spareRenderingBuffer);

#if defined(__arm__)
	// The chainloader runs with the MMU and caches disable so we need to make sure everything is flushed to the right
	// state before it starts
	invalidate_range_all_caches((uint32_t)buffer, (uint32_t)buffer + (uint32_t)buf_size);
	invalidate_range_all_caches(user_code_start, user_code_start + (uint32_t)code_size + 4);
	invalidate_range_all_caches((uint32_t)funcbuf, (uint32_t)funcbuf + 1024);

	// Jump to the chainloader. Bind the arguments to r0-r4 directly: the previous "five inputs plus
	// r0-r4 clobbered" form needed ten free registers and fails under LTO ("asm operand has impossible
	// constraints") as soon as this function grows.
	register uint32_t a0 asm("r0") = user_code_start;
	register uint32_t a1 asm("r1") = (uint32_t)code_size;
	register uint32_t a2 asm("r2") = user_code_exec;
	register uint32_t a3 asm("r3") = (uint32_t)buffer;
	register uint32_t a4 asm("r4") = (uint32_t)funcbuf;
	asm volatile("blx deluge_chainload" : : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4) : "memory");
#endif
}
