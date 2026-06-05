#include "menu.h"
#include <stdio.h>
#include <string.h>

// Save state menu state
volatile int      ss_state        = SS_MENU_CLOSED;
volatile int      ss_slot         = 0;
volatile int      ss_cursor       = SS_SAVE;
bool              ss_exists[10]   = {false};
char              ss_toast[32]    = {0};
int               ss_toast_f      = 0;

// 5x7 pixel font (ASCII 32-127, sparse)
static const uint8_t SS_FONT[128][5] = {
    [' ']={0,0,0,0,0},        ['!']={0,0,0x5F,0,0},
    ['0']={0x3E,0x51,0x49,0x45,0x3E}, ['1']={0,0x42,0x7F,0x40,0},
    ['2']={0x42,0x61,0x51,0x49,0x46}, ['3']={0x21,0x41,0x45,0x4B,0x31},
    ['4']={0x18,0x14,0x12,0x7F,0x10}, ['5']={0x27,0x45,0x45,0x45,0x39},
    ['6']={0x3C,0x4A,0x49,0x49,0x30}, ['7']={0x01,0x71,0x09,0x05,0x03},
    ['8']={0x36,0x49,0x49,0x49,0x36}, ['9']={0x06,0x49,0x49,0x29,0x1E},
    ['A']={0x7E,0x11,0x11,0x11,0x7E}, ['B']={0x7F,0x49,0x49,0x49,0x36},
    ['C']={0x3E,0x41,0x41,0x41,0x22}, ['D']={0x7F,0x41,0x41,0x22,0x1C},
    ['E']={0x7F,0x49,0x49,0x49,0x41}, ['F']={0x7F,0x09,0x09,0x09,0x01},
    ['G']={0x3E,0x41,0x49,0x49,0x7A}, ['H']={0x7F,0x08,0x08,0x08,0x7F},
    ['I']={0,0x41,0x7F,0x41,0},       ['J']={0x20,0x40,0x41,0x3F,0x01},
    ['K']={0x7F,0x08,0x14,0x22,0x41}, ['L']={0x7F,0x40,0x40,0x40,0x40},
    ['M']={0x7F,0x02,0x0C,0x02,0x7F}, ['N']={0x7F,0x04,0x08,0x10,0x7F},
    ['O']={0x3E,0x41,0x41,0x41,0x3E}, ['P']={0x7F,0x09,0x09,0x09,0x06},
    ['Q']={0x3E,0x41,0x51,0x21,0x5E}, ['R']={0x7F,0x09,0x19,0x29,0x46},
    ['S']={0x46,0x49,0x49,0x49,0x31}, ['T']={0x01,0x01,0x7F,0x01,0x01},
    ['U']={0x3F,0x40,0x40,0x40,0x3F}, ['V']={0x1F,0x20,0x40,0x20,0x1F},
    ['W']={0x3F,0x40,0x38,0x40,0x3F}, ['X']={0x63,0x14,0x08,0x14,0x63},
    ['Y']={0x07,0x08,0x70,0x08,0x07}, ['Z']={0x61,0x51,0x49,0x45,0x43},
    ['a']={0x20,0x54,0x54,0x54,0x78}, ['b']={0x7F,0x48,0x44,0x44,0x38},
    ['c']={0x38,0x44,0x44,0x44,0x20}, ['d']={0x38,0x44,0x44,0x48,0x7F},
    ['e']={0x38,0x54,0x54,0x54,0x18}, ['f']={0x08,0x7E,0x09,0x01,0x02},
    ['g']={0x0C,0x52,0x52,0x52,0x3E}, ['h']={0x7F,0x08,0x04,0x04,0x78},
    ['i']={0,0x44,0x7D,0x40,0},       ['j']={0x20,0x40,0x44,0x3D,0},
    ['k']={0x7F,0x10,0x28,0x44,0},    ['l']={0,0x41,0x7F,0x40,0},
    ['m']={0x7C,0x04,0x18,0x04,0x78}, ['n']={0x7C,0x08,0x04,0x04,0x78},
    ['o']={0x38,0x44,0x44,0x44,0x38}, ['p']={0x7C,0x14,0x14,0x14,0x08},
    ['q']={0x08,0x14,0x14,0x18,0x7C}, ['r']={0x7C,0x08,0x04,0x04,0x08},
    ['s']={0x48,0x54,0x54,0x54,0x20}, ['t']={0x04,0x3F,0x44,0x40,0x20},
    ['u']={0x3C,0x40,0x40,0x20,0x7C}, ['v']={0x1C,0x20,0x40,0x20,0x1C},
    ['w']={0x3C,0x40,0x30,0x40,0x3C}, ['x']={0x44,0x28,0x10,0x28,0x44},
    ['y']={0x0C,0x50,0x50,0x50,0x3C}, ['z']={0x44,0x64,0x54,0x4C,0x44},
    ['-']={0x08,0x08,0x08,0x08,0x08}, ['<']={0x08,0x14,0x22,0x41,0},
    ['>']={0,0x41,0x22,0x14,0x08},    ['.']={0,0x60,0x60,0,0},
    ['(']={0x1C,0x22,0x41,0,0},       [')']={0,0,0x41,0x22,0x1C},
};

// Drawing primitives (physical portrait coordinates)

static inline void ss_rect(uint16_t *p, int r0, int c_top, int rw, int ch, uint16_t col) {
    if (col == 0) {
        for (int r = r0; r < r0 + rw; r++)
            memset(p + r * PHYS_W + (c_top - ch + 1), 0, ch * 2);
        return;
    }
    uint32_t col32 = ((uint32_t)col << 16) | col;
    int pairs = ch >> 1, rem = ch & 1;
    for (int r = r0; r < r0 + rw; r++) {
        uint16_t *row   = p + r * PHYS_W + (c_top - ch + 1);
        uint32_t *row32 = (uint32_t *)row;
        for (int i = 0; i < pairs; i++) row32[i] = col32;
        if (rem) row[ch - 1] = col;
    }
}

static inline void ss_hline(uint16_t *p, int r0, int c, int rw, uint16_t col) {
    for (int r = r0; r < r0 + rw; r++) p[r * PHYS_W + c] = col;
}

static inline void ss_vline(uint16_t *p, int r, int c_top, int ch, uint16_t col) {
    for (int c = c_top - ch + 1; c <= c_top; c++) p[r * PHYS_W + c] = col;
}

static void ss_text(uint16_t *p, const char *s, int row, int col, int sc, uint16_t color) {
    for (int ci = 0; s[ci]; ci++) {
        unsigned char ch = (unsigned char)s[ci];
        if (ch >= 128) { row += 6 * sc; continue; }
        for (int fx = 0; fx < 5; fx++) {
            uint8_t bits = SS_FONT[ch][fx];
            for (int fy = 0; fy < 7; fy++) {
                if (!(bits & (1 << fy))) continue;
                for (int sy = 0; sy < sc; sy++)
                for (int sx = 0; sx < sc; sx++) {
                    int pr = row + fx * sc + sy;
                    int pc = col - fy * sc - sx;
                    if (pr >= 0 && pr < PHYS_H && pc >= 0 && pc < PHYS_W)
                        p[pr * PHYS_W + pc] = color;
                }
            }
        }
        row += 6 * sc;
    }
}

// Draw save state menu overlay

void draw_ss_menu(uint8_t *buf) {
    uint16_t *p = (uint16_t *)buf;
    const int SC = 2, CW = 6 * SC, CH = 8 * SC;
    const int R0 = SS_MENU_R0, C0 = SS_MENU_C0, RW = SS_MENU_RW, BH = SS_MENU_BH;

    ss_rect(p, R0, C0, RW, BH, 0x1083);
    ss_hline(p, R0,       C0,       RW, 0xF800);
    ss_hline(p, R0,       C0-BH+1,  RW, 0xF800);
    ss_vline(p, R0,       C0,       BH, 0xF800);
    ss_vline(p, R0+RW-1,  C0,       BH, 0xF800);
    ss_text(p, "SAVE STATE", R0+8, C0-8, SC, 0xFFFF);

    char slot_str[24];
    snprintf(slot_str, sizeof(slot_str), "< Slot %d >", (int)ss_slot);
    ss_text(p, slot_str, R0+8, C0-8-CH-8, SC, 0xF800);
    ss_text(p, ss_exists[ss_slot] ? "SAVED" : "EMPTY",
            R0+8+11*CW, C0-8-CH-8, SC, ss_exists[ss_slot] ? 0xF800 : 0x8410);

    const char *items[4] = {"SAVE", "LOAD", "DELETE", "CANCEL"};
    for (int i = 0; i < 4; i++) {
        int ic = C0 - 8 - (CH + 8) * (i + 2) - 8;
        uint16_t icol;
        if ((i == SS_LOAD || i == SS_DELETE) && !ss_exists[ss_slot])
            icol = 0x8410;
        else if (i == SS_DELETE && ss_exists[ss_slot])
            icol = 0xF800;
        else
            icol = 0xFFFF;
        if (ss_cursor == i) ss_text(p, ">", R0+8, ic, SC, 0xF800);
        ss_text(p, items[i], R0+8+CW*2, ic, SC, icol);
    }

    if (ss_toast_f > 0) {
        ss_toast_f--;
        int toast_col = C0 - 8 - (CH + 8) * 6 - 8;
        ss_text(p, ss_toast, R0+6, toast_col, SC, 0xFFFF);
    }
}

// Draw layout menu overlay

void draw_layout_menu(uint8_t *buf, int cursor, int current_layout) {
    uint16_t *p = (uint16_t *)buf;
    const int SC = 2, CW = 6 * SC, CH = 8 * SC;
    const int R0 = LM_R0, RW = LM_RW, C0 = LM_C0, BH = LM_BH;

    ss_rect(p, R0, C0, RW, BH, 0x1083);
    ss_hline(p, R0,       C0,       RW, 0xF800);
    ss_hline(p, R0,       C0-BH+1,  RW, 0xF800);
    ss_vline(p, R0,       C0,       BH, 0xF800);
    ss_vline(p, R0+RW-1,  C0,       BH, 0xF800);
    ss_text(p, "LAYOUT", R0+8, C0-8, SC, 0xFFFF);

    const char *labels[] = {"Default", "WASD"};
    for (int i = 0; i < 2; i++) {
        int ic = C0 - 8 - (CH + 8) * (i + 1);
        if (cursor == i) ss_text(p, ">", R0+8, ic, SC, 0xF800);
        uint16_t tcol = (current_layout == i) ? 0xF800 : 0xFFFF;
        ss_text(p, labels[i], R0+8+CW*2, ic, SC, tcol);
    }
}
