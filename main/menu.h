#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// Save state menu state
extern volatile int      ss_state;
extern volatile int      ss_slot;
extern volatile int      ss_cursor;
extern bool              ss_exists[10];
extern char              ss_toast[32];
extern int               ss_toast_f;

// Draw overlays onto physical render buffer
void draw_ss_menu(uint8_t *buf);
void draw_layout_menu(uint8_t *buf, int cursor, int current_layout);
