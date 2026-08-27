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

void chainload_from_buf(uint8_t* buffer, int buf_size) {
	uint32_t user_code_start = *(uint32_t*)(buffer + OFF_USER_CODE_START);
	uint32_t user_code_end = *(uint32_t*)(buffer + OFF_USER_CODE_END);
	uint32_t user_code_exec = *(uint32_t*)(buffer + OFF_USER_CODE_EXECUTE);

	int32_t code_size = (int32_t)(user_code_end - user_code_start);
	if (code_size > buf_size) {
		FREEZE_WITH_ERROR("E994");
		return;
	}

	// Disable interrupts so we don't get interrupted during the chainload
#if defined(__arm__)
	ENTER_CRITICAL_SECTION();
#endif
	// Disable timers
	disableTimer(TIMER_MIDI_GATE_OUTPUT);
	disableTimer(TIMER_SYSTEM_SLOW);
	disableTimer(TIMER_SYSTEM_FAST);
	disableTimer(TIMER_SYSTEM_SUPERFAST);
	uint8_t* funcbuf = reinterpret_cast<uint8_t*>(spareRenderingBuffer);

#if defined(__arm__)
	// The chainloader runs with the MMU and caches disable so we need to make sure everything is flushed to the right
	// state before it starts
	invalidate_range_all_caches((uint32_t)buffer, (uint32_t)buffer + (uint32_t)buf_size);
	invalidate_range_all_caches(user_code_start, user_code_start + (uint32_t)code_size + 4);
	invalidate_range_all_caches((uint32_t)funcbuf, (uint32_t)funcbuf + 1024);

	// The chainloader only switches off the L1 caches (SCTLR); the PL310 L2 stays enabled and keeps every line
	// allocated after the range maintenance above - this function, the cache routines, the first-stage loader.
	// Those lines hold the OLD image. The MMU-off copy bypasses L2, so once the new image turns its MMU back on
	// it can fetch stale old-image bytes at those addresses before it reaches its own L2CacheInit(). A byte-identical
	// image hides this; any other image can crash during early boot. Clean + invalidate the whole L2 and switch it
	// off so the new image starts from a cold L2, exactly as it would after a power-on.
	L2CacheCleanInvalidateAll();
	L2CacheDisable();

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
