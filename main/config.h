#pragma once

// GBA screen dimensions
#define GBA_WIDTH  240
#define GBA_HEIGHT 160

// Physical display buffer dimensions (portrait layout, landscape display)
#define PHYS_W 480
#define PHYS_H 800

// Save state menu state machine values
#define SS_MENU_CLOSED  0
#define SS_MENU_OPEN    1
#define SS_MENU_SAVING  2
#define SS_MENU_LOADING 3

// Save state cursor op codes
#define SS_SAVE   0
#define SS_LOAD   1
#define SS_DELETE 2
#define SS_CANCEL 3

// Save state menu rect (physical portrait coords)
#define SS_MENU_R0      560
#define SS_MENU_RW      220
#define SS_MENU_C0      460
#define SS_MENU_BH      190
#define SS_MENU_COL_LO  (SS_MENU_C0 - SS_MENU_BH + 1)
#define SS_MENU_COL_HI  (SS_MENU_C0 + 1)

// Layout menu rect (top-left quadrant in landscape)
#define LM_R0     50
#define LM_RW     145
#define LM_C0     350
#define LM_BH     110

// gpSP save state buffer size
#define GBA_STATE_BUF_SIZE  (416*1024)

