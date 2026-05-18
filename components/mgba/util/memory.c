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
	// Use PSRAM for large allocations (ROM data), internal RAM for small ones
	if (size >= 64 * 1024) {
		void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
		if (p) {
			memset(p, 0, size);
			return p;
		}
	}
#endif
	return calloc(1, size);
}

void mappedMemoryFree(void* memory, size_t size) {
	UNUSED(size);
	free(memory);
}
