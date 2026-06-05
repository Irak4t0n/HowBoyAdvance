#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include "bsp/device.h"
#include "bootloader_common.h"
#include "esp_system.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "portmacro.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_vfs_fat.h"
#include "esp_cache.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "targets/tanmatsu/tanmatsu_hardware.h"
#include "esp_heap_caps.h"
#include "sys/stat.h"
#include "esp_timer.h"
#include "bsp/audio.h"
#include "driver/i2s_std.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_pm.h"

// gpSP headers
#include "gpsp_esp.h"
#include "rom_selector.h"
#include "config.h"
#include "menu.h"

// ── Constants ─────────────────────────────────────────────────────────────────

static char const TAG[] = "howboy";

// ROMS_DIR and SAVES_DIR defined in rom_selector.h

// ── Global state ──────────────────────────────────────────────────────────────

static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
pax_buf_t                  fb_pax               = {0};
QueueHandle_t              input_event_queue    = NULL;

// gpSP framebuffer pointer (owned by gpSP)
static uint16_t *gba_fb = NULL;

// Task handles
static TaskHandle_t      emulator_task_handle = NULL;

// Input state
static volatile uint32_t gba_keys = 0;

// FPS tracking
static volatile int fps_val = 0;
static volatile bool show_fps = false;

// Audio pipeline
#define AUDIO_SAMPLE_RATE   65536  // gpSP native: GBA_SOUND_FREQUENCY = 64*1024
#define AUDIO_FRAMES_PER_VBLANK 1097  // 65536 / 59.7275 ≈ 1097 stereo frames per GBA frame
#define AUDIO_DRAIN_MAX     1200  // slightly over one frame's worth

static int16_t             *audio_buf_a  = NULL;
static int16_t             *audio_buf_b  = NULL;
static volatile int         audio_buf_len = 0;
static volatile int         audio_buf_ready = 0;
static SemaphoreHandle_t    sem_audio_ready = NULL;
static SemaphoreHandle_t    sem_audio_done  = NULL;
static volatile int         audio_samples_produced = 0;
static float                volume_level = 50.0f;
static volatile int         audio_mute = 0;

// Fast forward
static volatile int  ff_speed = 0;

// Button layout: 0 = Default (A/D = A/B), 1 = WASD (WASD = d-pad, ;/[ = A/B)
static volatile int  key_layout = 0;
static volatile int  layout_menu_open = 0;
static volatile int  lm_cursor = 0;

// Save path (for autosave + exit save)
static char sram_path_global[384] = {0};
static char state_save_dir[384]   = {0};

// Save state buffer (416KB, allocated on-demand to save PSRAM for ROM)
static void *state_buf = NULL;


// PPA hardware scaler
#define PPA_SCALE_X  3.3125f
#define PPA_SCALE_Y  3.0f
#define SCALED_W  480
#define SCALED_H  795
#define BORDER_Y  ((PHYS_H - SCALED_H) / 2)

static ppa_client_handle_t ppa_srm_client = NULL;
static uint8_t          *render_buf[1] = {NULL};
static SemaphoreHandle_t sem_frame_ready = NULL;
static SemaphoreHandle_t sem_scale_done  = NULL;

// ── Audio task (Core 0) ──────────────────────────────────────────────────────

static void audio_task(void *arg) {
    i2s_chan_handle_t i2s = NULL;
    bsp_audio_get_i2s_handle(&i2s);
    size_t written;
    static int16_t silence[2048] = {0};

    while (1) {
        xSemaphoreTake(sem_audio_ready, portMAX_DELAY);
        if (audio_mute) {
            // Write silence to keep I2S DMA flowing (prevents pops on unmute)
            if (i2s)
                i2s_channel_write(i2s, silence, audio_buf_len * 2 * sizeof(int16_t),
                                  &written, pdMS_TO_TICKS(100));
            xSemaphoreGive(sem_audio_done);
            continue;
        }
        int16_t *buf = (audio_buf_ready == 0) ? audio_buf_a : audio_buf_b;
        int len = audio_buf_len;
        if (len > 0 && i2s)
            i2s_channel_write(i2s, buf, len * 2 * sizeof(int16_t),
                              &written, pdMS_TO_TICKS(100));
        xSemaphoreGive(sem_audio_done);
    }
}

static void drain_and_submit_audio(void) {
    if (!audio_buf_a) return;
    int16_t *buf = (audio_buf_ready == 0) ? audio_buf_b : audio_buf_a;

    if (ff_speed > 0) {
        // Non-blocking silence during fast-forward
        if (xSemaphoreTake(sem_audio_done, 0) == pdTRUE) {
            memset(buf, 0, AUDIO_FRAMES_PER_VBLANK * 2 * sizeof(int16_t));
            audio_buf_len = AUDIO_FRAMES_PER_VBLANK;
            audio_buf_ready ^= 1;
            xSemaphoreGive(sem_audio_ready);
        }
        // Drain gpSP audio buffer to prevent buildup
        int16_t dummy[512];
        while (gpsp_get_audio(dummy, 256) > 0) {}
        return;
    }

    unsigned frames = gpsp_get_audio(buf, AUDIO_FRAMES_PER_VBLANK);
    // Pad remainder with silence so I2S always gets a full frame — prevents DMA underruns
    if (frames < AUDIO_FRAMES_PER_VBLANK)
        memset(buf + frames * 2, 0, (AUDIO_FRAMES_PER_VBLANK - frames) * 2 * sizeof(int16_t));
    audio_samples_produced += frames;
    // Block up to 30ms for previous buffer to finish — prevents dropped audio
    xSemaphoreTake(sem_audio_done, pdMS_TO_TICKS(30));
    audio_buf_len = AUDIO_FRAMES_PER_VBLANK;
    audio_buf_ready ^= 1;
    xSemaphoreGive(sem_audio_ready);
}

// ── FPS overlay ───────────────────────────────────────────────────────────────

static void draw_fps_overlay(uint8_t *buf) {
    if (!show_fps || fps_val <= 0) return;

    static const uint8_t fps_font[][5] = {
        {0x1F,0x11,0x11,0x11,0x1F},{0x00,0x12,0x1F,0x10,0x00}, // 0,1
        {0x1D,0x15,0x15,0x15,0x17},{0x11,0x15,0x15,0x15,0x1F}, // 2,3
        {0x07,0x04,0x04,0x04,0x1F},{0x17,0x15,0x15,0x15,0x1D}, // 4,5
        {0x1F,0x15,0x15,0x15,0x1D},{0x01,0x01,0x01,0x01,0x1F}, // 6,7
        {0x1F,0x15,0x15,0x15,0x1F},{0x17,0x15,0x15,0x15,0x1F}, // 8,9
    };

    char fps_str[8];
    snprintf(fps_str, sizeof(fps_str), "%d", fps_val);
    int fps_len = (int)strlen(fps_str);

    uint16_t *phys = (uint16_t *)buf;
    const int SC = 3, start_row = 790;

    for (int ci = 0; ci < fps_len; ci++) {
        char ch = fps_str[ci];
        if (ch < '0' || ch > '9') continue;
        int idx = ch - '0';

        int char_row = start_row - (fps_len - 1 - ci) * 6 * SC;
        for (int col = 0; col < 5; col++) {
            uint8_t bits = fps_font[idx][col];
            for (int row = 0; row < 5; row++) {
                if (!(bits & (1 << row))) continue;
                for (int sy = 0; sy < SC; sy++)
                for (int sx = 0; sx < SC; sx++) {
                    int px = char_row - (4 - col) * SC - sy;
                    int py = 4 + (4 - row) * SC + sx;
                    if (px >= 0 && px < PHYS_H && py >= 0 && py < PHYS_W)
                        phys[px * PHYS_W + py] = 0xF800;
                }
            }
        }
    }
}

// ── Scale+blit task (Core 0) ─────────────────────────────────────────────────

static void blit_task(void *arg) {
    size_t fb_bytes = PHYS_W * PHYS_H * sizeof(uint16_t);
    while (1) {
        xSemaphoreTake(sem_frame_ready, portMAX_DELAY);

        uint16_t *dst = (uint16_t *)render_buf[0];

        ppa_srm_oper_config_t srm = {
            .in = {
                .buffer     = gba_fb,
                .pic_w      = GBA_WIDTH,
                .pic_h      = GBA_HEIGHT,
                .block_w    = GBA_WIDTH,
                .block_h    = GBA_HEIGHT,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm     = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer       = dst,
                .buffer_size  = fb_bytes,
                .pic_w        = PHYS_W,
                .pic_h        = PHYS_H,
                .block_offset_x = 0,
                .block_offset_y = BORDER_Y,
                .srm_cm       = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
            .scale_x        = PPA_SCALE_X,
            .scale_y        = PPA_SCALE_Y,
            .mode           = PPA_TRANS_MODE_BLOCKING,
        };
        ppa_do_scale_rotate_mirror(ppa_srm_client, &srm);

        // Draw overlays after PPA scale
        draw_fps_overlay(render_buf[0]);

        if (ss_state == SS_MENU_OPEN)
            draw_ss_menu(render_buf[0]);

        // Show toast briefly after menu closes
        if (ss_state == SS_MENU_CLOSED && ss_toast_f > 0)
            draw_ss_menu(render_buf[0]);

        if (layout_menu_open)
            draw_layout_menu(render_buf[0], lm_cursor, key_layout);

        xSemaphoreGive(sem_scale_done);
        bsp_display_blit(0, 0, display_h_res, display_v_res, render_buf[0]);
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void restart_to_launcher(void) {
    rtc_retain_mem_t *mem = bootloader_common_get_rtc_retain_mem();
    memset(mem->custom, 0, sizeof(mem->custom));
    esp_restart();
}

void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb_pax));
}

// ── Rewind ────────────────────────────────────────────────────────────────────


static void save_sram(void) {
    if (sram_path_global[0]) {
        gpsp_write_save(sram_path_global);
        ESP_LOGI(TAG, "SRAM saved: %s", sram_path_global);
    }
}

static void save_and_exit_launcher(void) {
    save_sram();
    bsp_audio_set_amplifier(false);
    bsp_audio_set_volume(0);
    vTaskDelay(pdMS_TO_TICKS(100));
    restart_to_launcher();
}

static void save_and_return_selector(void) {
    save_sram();
    bsp_audio_set_amplifier(false);
    bsp_audio_set_volume(0);
    vTaskDelay(pdMS_TO_TICKS(100));
    // Restart app (without launcher flag → boots back into HowBoyAdvance)
    esp_restart();
}

// ── Save state operations (inline on Core 1) ─────────────────────────────────

static void ss_ensure_buf(void) {
    if (!state_buf)
        state_buf = heap_caps_malloc(GBA_STATE_BUF_SIZE, MALLOC_CAP_SPIRAM);
}

static void ss_do_save(int slot) {
    ss_ensure_buf();
    if (!state_buf) { snprintf(ss_toast, sizeof(ss_toast), "No memory!"); ss_toast_f = 120; ss_state = SS_MENU_CLOSED; return; }
    gpsp_save_state_buf(state_buf);
    char path[400];
    snprintf(path, sizeof(path), "%s.ss%d", state_save_dir, slot);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(state_buf, 1, GBA_STATE_BUF_SIZE, f);
        fclose(f);
        ss_exists[slot] = true;
        snprintf(ss_toast, sizeof(ss_toast), "Slot %d saved!", slot);
        ESP_LOGI(TAG, "State saved: %s", path);
    } else {
        snprintf(ss_toast, sizeof(ss_toast), "Save failed!");
        ESP_LOGE(TAG, "Failed to save state: %s", path);
    }
    ss_toast_f = 120;
    ss_state = SS_MENU_CLOSED;
}

static void ss_do_load(int slot) {
    ss_ensure_buf();
    if (!state_buf) { snprintf(ss_toast, sizeof(ss_toast), "No memory!"); ss_toast_f = 120; ss_state = SS_MENU_CLOSED; return; }
    char path[400];
    snprintf(path, sizeof(path), "%s.ss%d", state_save_dir, slot);
    FILE *f = fopen(path, "rb");
    if (f) {
        fread(state_buf, 1, GBA_STATE_BUF_SIZE, f);
        fclose(f);
        if (gpsp_load_state_buf(state_buf)) {
            snprintf(ss_toast, sizeof(ss_toast), "Slot %d loaded!", slot);
            ESP_LOGI(TAG, "State loaded: %s", path);
        } else {
            snprintf(ss_toast, sizeof(ss_toast), "Load failed!");
            ESP_LOGE(TAG, "Invalid state: %s", path);
        }
    } else {
        snprintf(ss_toast, sizeof(ss_toast), "Slot %d empty!", slot);
    }
    ss_toast_f = 120;
    ss_state = SS_MENU_CLOSED;
}

static void ss_do_delete(int slot) {
    char path[400];
    snprintf(path, sizeof(path), "%s.ss%d", state_save_dir, slot);
    if (remove(path) == 0) {
        ss_exists[slot] = false;
        snprintf(ss_toast, sizeof(ss_toast), "Slot %d deleted!", slot);
        ESP_LOGI(TAG, "State deleted: %s", path);
    } else {
        snprintf(ss_toast, sizeof(ss_toast), "Delete failed!");
    }
    ss_toast_f = 120;
    ss_state = SS_MENU_CLOSED;
}

// ── Input handling ────────────────────────────────────────────────────────────

#define GBA_KEY_A      (1 << 0)
#define GBA_KEY_B      (1 << 1)
#define GBA_KEY_SELECT (1 << 2)
#define GBA_KEY_START  (1 << 3)
#define GBA_KEY_RIGHT  (1 << 4)
#define GBA_KEY_LEFT   (1 << 5)
#define GBA_KEY_UP     (1 << 6)
#define GBA_KEY_DOWN   (1 << 7)
#define GBA_KEY_R      (1 << 8)
#define GBA_KEY_L      (1 << 9)

// Pending save state operation (set by input, executed in main loop)
static volatile int ss_pending_op = 0;  // 0=none, 1=save, 2=load, 3=delete

static void poll_input(void) {
    bsp_input_event_t ev;

    // Scancode polling for A/B and d-pad (layout-dependent)
    if (!layout_menu_open) {
        bool st;
        if (key_layout == 1) {
            // WASD layout: WASD = d-pad, semicolon = A, left brace = B
            bsp_input_read_scancode(BSP_INPUT_SCANCODE_W,         &st); if (st) gba_keys |= GBA_KEY_UP;    else gba_keys &= ~GBA_KEY_UP;
            bsp_input_read_scancode(BSP_INPUT_SCANCODE_S,         &st); if (st) gba_keys |= GBA_KEY_DOWN;  else gba_keys &= ~GBA_KEY_DOWN;
            bsp_input_read_scancode(BSP_INPUT_SCANCODE_A,         &st); if (st) gba_keys |= GBA_KEY_LEFT;  else gba_keys &= ~GBA_KEY_LEFT;
            bsp_input_read_scancode(BSP_INPUT_SCANCODE_D,         &st); if (st) gba_keys |= GBA_KEY_RIGHT; else gba_keys &= ~GBA_KEY_RIGHT;
            bsp_input_read_scancode(BSP_INPUT_SCANCODE_SEMICOLON, &st); if (st) gba_keys |= GBA_KEY_A;     else gba_keys &= ~GBA_KEY_A;
            bsp_input_read_scancode(BSP_INPUT_SCANCODE_LEFTBRACE, &st); if (st) gba_keys |= GBA_KEY_B;     else gba_keys &= ~GBA_KEY_B;
        } else {
            // Default layout: A = GBA A, D = GBA B (matching HowBoyMatsu)
            bsp_input_read_scancode(BSP_INPUT_SCANCODE_A, &st); if (st) gba_keys |= GBA_KEY_A; else gba_keys &= ~GBA_KEY_A;
            bsp_input_read_scancode(BSP_INPUT_SCANCODE_D, &st); if (st) gba_keys |= GBA_KEY_B; else gba_keys &= ~GBA_KEY_B;
        }
        // L/R/Start/Select always polled via scancode
        bsp_input_read_scancode(BSP_INPUT_SCANCODE_Q,     &st); if (st) gba_keys |= GBA_KEY_L;      else gba_keys &= ~GBA_KEY_L;
        bsp_input_read_scancode(BSP_INPUT_SCANCODE_E,     &st); if (st) gba_keys |= GBA_KEY_R;      else gba_keys &= ~GBA_KEY_R;
        bsp_input_read_scancode(BSP_INPUT_SCANCODE_ENTER, &st); if (st) gba_keys |= GBA_KEY_START;  else gba_keys &= ~GBA_KEY_START;
        bsp_input_read_scancode(BSP_INPUT_SCANCODE_SPACE, &st); if (st) gba_keys |= GBA_KEY_SELECT; else gba_keys &= ~GBA_KEY_SELECT;
    }

    while (xQueueReceive(input_event_queue, &ev, 0) == pdTRUE) {
        if (ev.type == INPUT_EVENT_TYPE_NAVIGATION) {
            int pressed = ev.args_navigation.state;

            // Save state menu input
            if (ss_state == SS_MENU_OPEN && pressed) {
                switch (ev.args_navigation.key) {
                    case BSP_INPUT_NAVIGATION_KEY_UP:
                        ss_cursor--;
                        if (ss_cursor < SS_SAVE) ss_cursor = SS_CANCEL;
                        if ((ss_cursor == SS_LOAD || ss_cursor == SS_DELETE) && !ss_exists[ss_slot])
                            ss_cursor = SS_SAVE;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_DOWN:
                        ss_cursor++;
                        if (ss_cursor > SS_CANCEL) ss_cursor = SS_SAVE;
                        if ((ss_cursor == SS_LOAD || ss_cursor == SS_DELETE) && !ss_exists[ss_slot])
                            ss_cursor = SS_CANCEL;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_LEFT:
                        ss_slot--;
                        if (ss_slot < 0) ss_slot = 9;
                        if ((ss_cursor == SS_LOAD || ss_cursor == SS_DELETE) && !ss_exists[ss_slot])
                            ss_cursor = SS_SAVE;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_RIGHT:
                        ss_slot++;
                        if (ss_slot > 9) ss_slot = 0;
                        if ((ss_cursor == SS_LOAD || ss_cursor == SS_DELETE) && !ss_exists[ss_slot])
                            ss_cursor = SS_SAVE;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_RETURN:
                        if (ss_cursor == SS_CANCEL) {
                            ss_state = SS_MENU_CLOSED;
                        } else if (ss_cursor == SS_SAVE) {
                            ss_pending_op = 1;
                        } else if (ss_cursor == SS_LOAD && ss_exists[ss_slot]) {
                            ss_pending_op = 2;
                        } else if (ss_cursor == SS_DELETE && ss_exists[ss_slot]) {
                            ss_pending_op = 3;
                        }
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_F4:
                        ss_state = SS_MENU_CLOSED;
                        break;
                    default: break;
                }
                continue;
            }

            // Layout menu input
            if (layout_menu_open && pressed) {
                switch (ev.args_navigation.key) {
                    case BSP_INPUT_NAVIGATION_KEY_UP:
                        lm_cursor = 0; break;
                    case BSP_INPUT_NAVIGATION_KEY_DOWN:
                        lm_cursor = 1; break;
                    case BSP_INPUT_NAVIGATION_KEY_RETURN:
                        key_layout = lm_cursor;
                        layout_menu_open = 0;
                        gba_keys = 0;  // release all keys on layout change
                        ESP_LOGI(TAG, "Layout: %s", key_layout ? "WASD" : "Default");
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_F2:
                        layout_menu_open = 0; break;
                    default: break;
                }
                continue;
            }

            // Normal gameplay — d-pad via navigation events (default layout only)
            if (key_layout == 0) {
                switch (ev.args_navigation.key) {
                    case BSP_INPUT_NAVIGATION_KEY_UP:
                        if (pressed) gba_keys |= GBA_KEY_UP;    else gba_keys &= ~GBA_KEY_UP;    break;
                    case BSP_INPUT_NAVIGATION_KEY_DOWN:
                        if (pressed) gba_keys |= GBA_KEY_DOWN;  else gba_keys &= ~GBA_KEY_DOWN;  break;
                    case BSP_INPUT_NAVIGATION_KEY_LEFT:
                        if (pressed) gba_keys |= GBA_KEY_LEFT;  else gba_keys &= ~GBA_KEY_LEFT;  break;
                    case BSP_INPUT_NAVIGATION_KEY_RIGHT:
                        if (pressed) gba_keys |= GBA_KEY_RIGHT; else gba_keys &= ~GBA_KEY_RIGHT; break;
                    default: break;
                }
            }

            // System keys (always active)
            switch (ev.args_navigation.key) {
                case BSP_INPUT_NAVIGATION_KEY_ESC:
                    if (pressed) save_and_exit_launcher();
                    break;

                case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
                    if (pressed) save_and_return_selector();
                    break;

                case BSP_INPUT_NAVIGATION_KEY_VOLUME_UP:
                    if (pressed) {
                        volume_level += 5.0f;
                        if (volume_level > 100.0f) volume_level = 100.0f;
                        bsp_audio_set_volume(volume_level);
                        ESP_LOGI(TAG, "Volume: %.0f%%", volume_level);
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN:
                    if (pressed) {
                        volume_level -= 5.0f;
                        if (volume_level < 0.0f) volume_level = 0.0f;
                        bsp_audio_set_volume(volume_level);
                        ESP_LOGI(TAG, "Volume: %.0f%%", volume_level);
                    }
                    break;

                case BSP_INPUT_NAVIGATION_KEY_F1:
                    if (pressed && ss_state == SS_MENU_CLOSED && !layout_menu_open) {
                        gpsp_reset();
                        ESP_LOGI(TAG, "Soft reset");
                    }
                    break;

                case BSP_INPUT_NAVIGATION_KEY_F2:
                    if (pressed && ss_state == SS_MENU_CLOSED) {
                        lm_cursor = key_layout;
                        layout_menu_open = 1;
                    }
                    break;

                case BSP_INPUT_NAVIGATION_KEY_F4:
                    if (pressed && ss_state == SS_MENU_CLOSED && !layout_menu_open &&
                        state_save_dir[0]) {
                        for (int si = 0; si < 10; si++) {
                            char spath[400];
                            snprintf(spath, sizeof(spath), "%s.ss%d", state_save_dir, si);
                            struct stat st;
                            ss_exists[si] = (stat(spath, &st) == 0);
                        }
                        ss_cursor = SS_SAVE;
                        ss_state  = SS_MENU_OPEN;
                    }
                    break;


                case BSP_INPUT_NAVIGATION_KEY_F6:
                    if (pressed) {
                        ff_speed = (ff_speed + 1) % 3;
                        ESP_LOGI(TAG, "FF: %s", (const char*[]){"OFF","5x","8x"}[ff_speed]);
                    }
                    break;

                default: break;
            }

        } else if (ev.type == INPUT_EVENT_TYPE_KEYBOARD) {
            // Backtick toggles FPS overlay
            if (ev.args_keyboard.ascii == '`') {
                show_fps = !show_fps;
                if (!show_fps) {
                    uint16_t *p = (uint16_t *)render_buf[0];
                    if (p) {
                        for (int r = 770; r < 800; r++)
                            for (int c = 0; c < 30; c++)
                                p[r * PHYS_W + c] = 0;
                    }
                }
            }
        }
    }
}

// ── Emulator task (Core 1) ────────────────────────────────────────────────────

static void emulator_task(void *arg) {
    // ── Mount SD card ────────────────────────────────────────────────────
    static int sd_mounted = 0;
    if (sd_mounted) goto sd_ready;

    {
        ESP_LOGI(TAG, "Mounting SD card...");
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.clk   = BSP_SDCARD_CLK;
        slot_config.cmd   = BSP_SDCARD_CMD;
        slot_config.d0    = BSP_SDCARD_D0;
        slot_config.d1    = BSP_SDCARD_D1;
        slot_config.d2    = BSP_SDCARD_D2;
        slot_config.d3    = BSP_SDCARD_D3;
        slot_config.width = 4;
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024,
        };
        sdmmc_card_t *card;
        esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
            pax_background(&fb_pax, 0xFF000000);
            pax_draw_text(&fb_pax, 0xFFFF0000, pax_font_sky_mono, 16, 10, 10, "SD card mount failed!");
            pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 40, "Press ESC to return");
            blit();
            goto wait_exit;
        }
        ESP_LOGI(TAG, "SD card mounted");
        sd_mounted = 1;
    }
sd_ready:;

    // ── Select ROM ────────────────────────────────────────────────────────
    scan_roms();
    const char *rom_path;
    if (rom_count == 0) {
        pax_background(&fb_pax, 0xFF000000);
        pax_draw_text(&fb_pax, 0xFFFF0000, pax_font_sky_mono, 16, 10, 10, "No GBA ROMs found!");
        pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 40, "Place .gba files in /sdcard/roms/");
        pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 70, "Press ESC to return");
        blit();
        goto wait_exit;
    } else {
        rom_path = rom_selector();
        if (!rom_path) goto wait_exit;
    }

    ESP_LOGI(TAG, "Loading ROM: %s", rom_path);
    pax_background(&fb_pax, 0xFF000000);
    pax_draw_text(&fb_pax, 0xFFFFFF00, pax_font_sky_mono, 14, 10, 10, "Loading ROM (gpSP)...");
    blit();

    // Free PAX buffer to maximize PSRAM for ROM
    pax_buf_destroy(&fb_pax);

    // Allocate render buffer BEFORE ROM load (avoids PSRAM fragmentation)
    {
        size_t fb_size = PHYS_W * PHYS_H * 2;
        render_buf[0] = heap_caps_aligned_alloc(64, fb_size, MALLOC_CAP_SPIRAM);
        if (!render_buf[0]) {
            ESP_LOGE(TAG, "Failed to allocate render buffer");
            goto wait_exit;
        }
        memset(render_buf[0], 0, fb_size);
        ESP_LOGI(TAG, "Render buf allocated: %u bytes", (unsigned)fb_size);
    }

    // Save state buffer allocated on-demand when needed (saves 416KB PSRAM for ROM)


    // ── Initialize gpSP ──────────────────────────────────────────────────
    if (!gpsp_init()) {
        ESP_LOGE(TAG, "gpSP init failed");
        goto wait_exit;
    }

    if (gpsp_load_rom(rom_path) != 0) {
        ESP_LOGE(TAG, "gpSP failed to load ROM (free PSRAM: %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        goto wait_exit;
    }
    ESP_LOGI(TAG, "ROM loaded via gpSP: %s (free PSRAM: %u)", rom_path,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Derive save/state paths from ROM filename
    mkdir(SAVES_DIR, 0777);
    {
        const char *base = strrchr(rom_path, '/');
        base = base ? base + 1 : rom_path;
        snprintf(sram_path_global, sizeof(sram_path_global), "%s/%s", SAVES_DIR, base);
        char *dot = strrchr(sram_path_global, '.');
        if (dot) strcpy(dot, ".sav");
        gpsp_load_save(sram_path_global);
        ESP_LOGI(TAG, "Save file: %s", sram_path_global);

        // State save dir: /sdcard/saves/ROMNAME (no extension)
        snprintf(state_save_dir, sizeof(state_save_dir), "%s/%s", SAVES_DIR, base);
        dot = strrchr(state_save_dir, '.');
        if (dot) *dot = '\0';
    }


    // Initialize PPA hardware scaler
    {
        ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        esp_err_t ret = ppa_register_client(&ppa_cfg, &ppa_srm_client);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PPA init failed: %s", esp_err_to_name(ret));
            goto wait_exit;
        }
        ESP_LOGI(TAG, "PPA scaler initialized (%.4fx/%.1fx + 90 CW -> %dx%d centered in %dx%d)",
                 PPA_SCALE_X, PPA_SCALE_Y, SCALED_W, SCALED_H, PHYS_W, PHYS_H);
    }

    // Start blit task on Core 0
    sem_frame_ready = xSemaphoreCreateBinary();
    sem_scale_done  = xSemaphoreCreateBinary();
    xSemaphoreGive(sem_scale_done);
    xTaskCreatePinnedToCore(blit_task, "blit", 4096, NULL, 6, NULL, 0);

    // Initialize audio pipeline
    {
        size_t buf_bytes = AUDIO_DRAIN_MAX * 2 * sizeof(int16_t);
        audio_buf_a = malloc(buf_bytes);
        audio_buf_b = malloc(buf_bytes);
        sem_audio_ready = xSemaphoreCreateBinary();
        sem_audio_done  = xSemaphoreCreateBinary();
        if (audio_buf_a && audio_buf_b && sem_audio_ready && sem_audio_done) {
            xSemaphoreGive(sem_audio_done);
            xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 7, NULL, 0);
            bsp_audio_set_rate(AUDIO_SAMPLE_RATE);
            // Pre-fill I2S DMA with silence to build buffer ahead (reduces underrun risk)
            {
                i2s_chan_handle_t i2s_pre = NULL;
                bsp_audio_get_i2s_handle(&i2s_pre);
                if (i2s_pre) {
                    int16_t silence[512] = {0};
                    size_t wr;
                    for (int i = 0; i < 4; i++)
                        i2s_channel_write(i2s_pre, silence, sizeof(silence), &wr, pdMS_TO_TICKS(50));
                }
            }
            bsp_audio_set_volume(volume_level);
            bsp_audio_set_amplifier(true);
            ESP_LOGI(TAG, "Audio initialized: %d Hz stereo", AUDIO_SAMPLE_RATE);
        } else {
            ESP_LOGW(TAG, "Audio disabled: allocation failed");
            free(audio_buf_a); audio_buf_a = NULL;
            free(audio_buf_b); audio_buf_b = NULL;
        }
    }

    // Lock CPU to max frequency
    esp_pm_lock_handle_t pm_lock;
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "emu", &pm_lock);
    esp_pm_lock_acquire(pm_lock);

    ESP_LOGI(TAG, "gpSP core initialized — starting emulation");

    // ── Main emulation loop ──────────────────────────────────────────────
    {
        // GBA frame period: 280896 cycles / 16.78MHz ≈ 16742.7 µs ≈ 59.7275 Hz
        #define FRAME_PERIOD_US  16743
        extern uint32_t skip_next_frame;
        int64_t fps_timer = esp_timer_get_time();
        int64_t next_frame = fps_timer;
        int frame_count = 0;
        int total_frames = 0;

        while (1) {
            // Handle pending save state operations (must run on Core 1)
            if (ss_pending_op) {
                int op = ss_pending_op;
                int slot = ss_slot;
                ss_pending_op = 0;
                if (op == 1) ss_do_save(slot);
                else if (op == 2) ss_do_load(slot);
                else if (op == 3) ss_do_delete(slot);
            }

            poll_input();

            // ── Normal emulation ──────────────────────────────────────
            gpsp_set_buttons(gba_keys);

            int frame_was_skipped = skip_next_frame;

            // Wait for PPA to finish reading gba_fb from previous frame
            xSemaphoreTake(sem_scale_done, portMAX_DELAY);

            // Run one frame of emulation
            gba_fb = gpsp_run_frame();

            if (!frame_was_skipped) {
                // Flush framebuffer from D-cache so PPA DMA sees updated pixels
                esp_cache_msync(gba_fb, GBA_WIDTH * GBA_HEIGHT * sizeof(uint16_t),
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                // Signal blit task to PPA scale + display
                xSemaphoreGive(sem_frame_ready);
            } else {
                // Skipped render — no blit needed, re-grant permission
                xSemaphoreGive(sem_scale_done);
            }
            drain_and_submit_audio();

            // Fast forward: skip frame pacing entirely
            static int ff_frame = 0;
            static const int ff_skip[] = {0, 4, 7};
            if (ff_speed > 0) {
                int skip = ff_skip[ff_speed];
                ff_frame++;
                if (ff_frame > skip) {
                    ff_frame = 0;
                    skip_next_frame = 0;
                } else {
                    skip_next_frame = 1;
                }
                // No frame pacing during FF
                frame_count++;
                total_frames++;
                int64_t now = esp_timer_get_time();
                if (now - fps_timer >= 1000000) {
                    fps_val = frame_count;
                    ESP_LOGI(TAG, "FPS: %d (FF %s)", fps_val,
                             (const char*[]){"OFF","5x","8x"}[ff_speed]);
                    frame_count = 0;
                    fps_timer = now;
                }
                // Reset frame pacing deadline
                next_frame = esp_timer_get_time();
                continue;
            }
            ff_frame = 0;

            // Frame pacing + auto frameskip
            next_frame += FRAME_PERIOD_US;
            int64_t now = esp_timer_get_time();
            int64_t wait_us = next_frame - now;

            if (wait_us > 1000) {
                vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
            }

            // Auto frameskip: if behind, skip next render (never 2 in a row)
            if (wait_us < 0 && !frame_was_skipped) {
                skip_next_frame = 1;
            } else {
                skip_next_frame = 0;
            }

            // Reset deadline if too far behind
            if (next_frame < now - FRAME_PERIOD_US * 3)
                next_frame = now;

            frame_count++;
            total_frames++;
            now = esp_timer_get_time();
            if (now - fps_timer >= 1000000) {
                fps_val = frame_count;
                ESP_LOGI(TAG, "FPS: %d", fps_val);
                audio_samples_produced = 0;
                frame_count = 0;
                fps_timer = now;
            }


            // Autosave SRAM every ~5 minutes (~18000 frames at 60fps)
            if (total_frames % 18000 == 0 && total_frames > 0) {
                save_sram();
            }
        }
    }

wait_exit:;
    {
        bsp_input_event_t ev;
        while (1) {
            if (xQueueReceive(input_event_queue, &ev, portMAX_DELAY) == pdTRUE) {
                if (ev.type == INPUT_EVENT_TYPE_NAVIGATION &&
                    ev.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_ESC &&
                    ev.args_navigation.state)
                    restart_to_launcher();
            }
        }
    }
}

// ── App entry point ───────────────────────────────────────────────────────────

void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    const bsp_configuration_t bsp_configuration = {
        .display = {
            .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB,
            .num_fbs = 1,
        },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) { ESP_LOGE(TAG, "BSP init failed: %d", res); return; }

    res = bsp_display_get_parameters(&display_h_res, &display_v_res,
                                      &display_color_format, &display_data_endian);
    if (res != ESP_OK) { ESP_LOGE(TAG, "Display params failed: %d", res); return; }

    pax_buf_type_t format = PAX_BUF_16_565RGB;
    bsp_display_rotation_t display_rotation = BSP_DISPLAY_ROTATION_90;
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:  orientation = PAX_O_ROT_CW;   break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CCW;  break;
        default: break;
    }

    pax_buf_init(&fb_pax, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb_pax, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
    pax_buf_set_orientation(&fb_pax, orientation);

    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    pax_background(&fb_pax, 0xFF000000);
    pax_draw_text(&fb_pax, 0xFF00FF00, pax_font_sky_mono, 16, 10, 10, "HowBoyAdvance (gpSP)");
    blit();

    xTaskCreatePinnedToCore(emulator_task, "emu", 48 * 1024, NULL, 5, &emulator_task_handle, 1);
}
