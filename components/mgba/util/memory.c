/* Copyright (c) 2013-2020 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba-util/memory.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

void* anonymousMemoryMap(size_t size) {
#ifdef ESP_PLATFORM
	// Push buffers >= 64KB to PSRAM (EWRAM 256KB, VRAM 96KB, ROM 16MB+).
	// Keeps IWRAM (32KB) and smaller buffers in fast internal SRAM.
	if (size >= 64 * 1024) {
		void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
		if (p) {
			memset(p, 0, size);
			return p;
		}
	}
	// Internal RAM for smaller allocations (VRAM 96KB, IWRAM 32KB, palette, OAM)
	void *p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
	if (p) {
		memset(p, 0, size);
		return p;
	}
#endif
	return calloc(1, size);
}

void mappedMemoryFree(void* memory, size_t size) {
	UNUSED(size);
	free(memory);
}
