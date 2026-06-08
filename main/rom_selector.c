#include "rom_selector.h"

#include <stdlib.h>
#include "esp_attr.h"
#include <string.h>
#include <dirent.h>
#include "esp_log.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "bsp/input.h"
#include "bsp/display.h"
#include "bsp/audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// From main.c
extern pax_buf_t     fb_pax;
extern QueueHandle_t input_event_queue;
void blit(void);
void restart_to_launcher(void);

// ── ROM list ──────────────────────────────────────────────────────────────────
EXT_RAM_BSS_ATTR char rom_list[MAX_ROMS][300];
int  rom_count = 0;

// ── 5x7 bitmap font for direct-pixel text rendering ──────────────────────────
static const uint8_t rom_font5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x08,0x2A,0x1C,0x2A,0x08}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x08,0x14,0x22,0x41,0x00}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x00,0x41,0x22,0x14,0x08}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x49,0x49,0x7A}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x04,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x00,0x7F,0x41,0x41,0x00}, // [
    {0x02,0x04,0x08,0x10,0x20}, // backslash
    {0x00,0x41,0x41,0x7F,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04}, // ^
    {0x40,0x40,0x40,0x40,0x40}, // _
    {0x00,0x01,0x02,0x04,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7F,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7F}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {0x08,0x7E,0x09,0x01,0x02}, // f
    {0x0C,0x52,0x52,0x52,0x3E}, // g
    {0x7F,0x08,0x04,0x04,0x78}, // h
    {0x00,0x44,0x7D,0x40,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00}, // j
    {0x7F,0x10,0x28,0x44,0x00}, // k
    {0x00,0x41,0x7F,0x40,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78}, // m
    {0x7C,0x08,0x04,0x04,0x78}, // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7C,0x14,0x14,0x14,0x08}, // p
    {0x08,0x14,0x14,0x18,0x7C}, // q
    {0x7C,0x08,0x04,0x04,0x08}, // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {0x04,0x3F,0x44,0x40,0x20}, // t
    {0x3C,0x40,0x40,0x40,0x7C}, // u
    {0x1C,0x20,0x40,0x20,0x1C}, // v
    {0x3C,0x40,0x30,0x40,0x3C}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0C,0x50,0x50,0x50,0x3C}, // y
    {0x44,0x64,0x54,0x4C,0x44}, // z
    {0x00,0x08,0x36,0x41,0x00}, // {
    {0x00,0x00,0x7F,0x00,0x00}, // |
    {0x00,0x41,0x36,0x08,0x00}, // }
    {0x08,0x08,0x2A,0x1C,0x08}, // ~
    {0x00,0x00,0x00,0x00,0x00}, // del
};

// ── Direct-pixel text and fill (bypasses PAX rotation for speed) ──────────────

static void rom_draw_text_direct(const char *s, int lx, int ly, int sc, uint16_t color) {
    uint16_t *buf = (uint16_t *)pax_buf_get_pixels(&fb_pax);
    int cx = lx;
    for (int si = 0; s[si]; si++) {
        uint8_t c = (uint8_t)s[si];
        if (c < 32 || c > 127) { cx += 6 * sc; continue; }
        const uint8_t *glyph = rom_font5x7[c - 32];
        for (int fx = 0; fx < 5; fx++) {
            uint8_t col = glyph[fx];
            for (int fy = 0; fy < 7; fy++) {
                if (!(col & (1 << fy))) continue;
                for (int sx = 0; sx < sc; sx++)
                for (int sy = 0; sy < sc; sy++) {
                    int pr = cx + fx * sc + sx;
                    int pc = 479 - (ly + fy * sc + sy);
                    if (pr >= 0 && pr < 800 && pc >= 0 && pc < 480)
                        buf[pr * 480 + pc] = color;
                }
            }
        }
        cx += 6 * sc;
    }
}

static void rom_fill_row_direct(int row_y, int row_h, uint16_t color565) {
    uint16_t *buf = (uint16_t *)pax_buf_get_pixels(&fb_pax);
    int col_start = 479 - row_y - row_h + 1;
    int col_end   = 479 - row_y;
    if (col_start < 0) col_start = 0;
    if (col_end > 479) col_end = 479;
    for (int r = 0; r < 800; r++)
        for (int c = col_start; c <= col_end; c++)
            buf[r * 480 + c] = color565;
}

static void rom_selector_draw_row(int idx, int scroll, int selected, int LIST_Y, int ROW_H) {
    int i = idx - scroll;
    const char *fname = strrchr(rom_list[idx], '/');
    fname = fname ? fname + 1 : rom_list[idx];
    char display[120];
    strncpy(display, fname, sizeof(display) - 1);
    display[sizeof(display) - 1] = 0;
    char *dot = strrchr(display, '.');
    if (dot) *dot = 0;
    int row_y = LIST_Y + i * ROW_H;
    if (idx == selected)
        rom_draw_text_direct(display, 12, row_y + 6, 2, 0x0000);
    else
        rom_draw_text_direct(display, 12, row_y + 6, 2, 0xDEDB);
}

// ── ROM scanner ───────────────────────────────────────────────────────────────

void scan_roms(void) {
    rom_count = 0;
    DIR *dir = opendir(ROMS_DIR);
    if (!dir) { ESP_LOGE("rom_selector", "Cannot open roms dir"); return; }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && rom_count < MAX_ROMS) {
        char *name = entry->d_name;
        int len = strlen(name);
        if (len > 4) {
            char *ext = name + len - 4;
            if (strcasecmp(ext, ".gba") == 0) {
                snprintf(rom_list[rom_count], sizeof(rom_list[0]), "%s/%s", ROMS_DIR, name);
                rom_count++;
            }
        }
    }
    closedir(dir);
    qsort(rom_list, rom_count, sizeof(rom_list[0]),
          (int (*)(const void *, const void *))strcasecmp);
    ESP_LOGI("rom_selector", "Found %d ROMs", rom_count);
}

// ── ROM selector UI ───────────────────────────────────────────────────────────

const char *rom_selector(void) {
    if (rom_count == 0) return NULL;

    int selected = 0, scroll = 0;
    const int W = 800, H = 480;
    const int HEADER_H = 60, FOOTER_H = 36;
    const int LIST_Y = 68, ROW_H = 32;
    const int visible = (H - HEADER_H - FOOTER_H - 16) / ROW_H;

    bool full_redraw = true;
    int prev_selected = -1, prev_scroll = -1;

    while (1) {
        bool scroll_changed = (scroll != prev_scroll);

        if (full_redraw || scroll_changed) {
            pax_background(&fb_pax, 0xFF111111);
            pax_simple_rect(&fb_pax, 0xFF2E1A1A, 0, 0, W, HEADER_H);
            pax_draw_text(&fb_pax, 0xFFFF0000, pax_font_sky_mono, 28, 16, 14, "HowBoyAdvance");
            char counter[32];
            snprintf(counter, sizeof(counter), "%d/%d", selected + 1, rom_count);
            pax_draw_text(&fb_pax, 0xFF888888, pax_font_sky_mono, 18, W - 100, 20, counter);
            pax_simple_rect(&fb_pax, 0xFFFF0000, 0, HEADER_H, W, 2);
            for (int i = 0; i < visible && (scroll + i) < rom_count; i++) {
                int idx = scroll + i;
                uint16_t bg565;
                if (idx == selected)  bg565 = 0x8000;
                else if (i % 2 == 0)  bg565 = 0x1800;
                else                  bg565 = 0x2000;
                rom_fill_row_direct(LIST_Y + i * ROW_H, ROW_H, bg565);
            }
            for (int i = 0; i < visible && (scroll + i) < rom_count; i++)
                rom_selector_draw_row(scroll + i, scroll, selected, LIST_Y, ROW_H);
            pax_simple_rect(&fb_pax, 0xFF2E1A1A, 0, H - FOOTER_H, W, FOOTER_H);
            pax_simple_rect(&fb_pax, 0xFFFF0000, 0, H - FOOTER_H, W, 2);
            pax_draw_text(&fb_pax, 0xFF888888, pax_font_sky_mono, 14, 12, H - FOOTER_H + 10,
                          "[Up/Down] Navigate   [Enter/A] Launch   [ESC] Exit");
            full_redraw = false;
        } else if (prev_selected != selected) {
            pax_simple_rect(&fb_pax, 0xFF2E1A1A, W - 110, 10, 110, 30);
            char counter[32];
            snprintf(counter, sizeof(counter), "%d/%d", selected + 1, rom_count);
            pax_draw_text(&fb_pax, 0xFF888888, pax_font_sky_mono, 18, W - 100, 20, counter);
            if (prev_selected >= scroll && prev_selected < scroll + visible) {
                int pi = prev_selected - scroll;
                uint16_t bg = (pi % 2 == 0) ? 0x1800 : 0x2000;
                rom_fill_row_direct(LIST_Y + pi * ROW_H, ROW_H, bg);
                rom_selector_draw_row(prev_selected, scroll, selected, LIST_Y, ROW_H);
            }
            if (selected >= scroll && selected < scroll + visible) {
                rom_fill_row_direct(LIST_Y + (selected - scroll) * ROW_H, ROW_H, 0x8000);
                rom_selector_draw_row(selected, scroll, selected, LIST_Y, ROW_H);
            }
        }

        prev_selected = selected;
        prev_scroll   = scroll;
        blit();

        bsp_input_event_t ev;
        if (xQueueReceive(input_event_queue, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.type == INPUT_EVENT_TYPE_NAVIGATION && ev.args_navigation.state == 1) {
            switch (ev.args_navigation.key) {
                case BSP_INPUT_NAVIGATION_KEY_UP:
                    if (selected > 0) {
                        selected--;
                        if (selected < scroll) scroll = selected;
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_DOWN:
                    if (selected < rom_count - 1) {
                        selected++;
                        if (selected >= scroll + visible) scroll = selected - visible + 1;
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_RETURN:
                    return rom_list[selected];
                case BSP_INPUT_NAVIGATION_KEY_ESC:
                    bsp_audio_set_volume(0);
                    vTaskDelay(pdMS_TO_TICKS(150));
                    restart_to_launcher();
                    break;
                default: break;
            }
        } else if (ev.type == INPUT_EVENT_TYPE_KEYBOARD && ev.args_keyboard.ascii == 'a') {
            return rom_list[selected];
        }
    }
}
