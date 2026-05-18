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
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "targets/tanmatsu/tanmatsu_hardware.h"
#include "esp_heap_caps.h"
#include "dirent.h"
#include "sys/stat.h"
#include "esp_timer.h"

// mGBA headers
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/core/blip_buf.h>
#include <mgba/gba/core.h>
#include <mgba/gba/interface.h>
#include <mgba-util/vfs.h>

// ── Constants ─────────────────────────────────────────────────────────────────

static char const TAG[] = "howboy";

#define GBA_WIDTH   240
#define GBA_HEIGHT  160
#define PHYS_W      480   // portrait width  (physical display columns)
#define PHYS_H      800   // portrait height (physical display rows)
// After CW rotation: landscape is 800 wide × 480 tall
#define LAND_W      PHYS_H  // 800
#define LAND_H      PHYS_W  // 480
// GBA 240×160 → 800×480 fill: Y is 3x exact, X is 3.33x (80 of 240 get 4 rows)

#define ROMS_DIR    "/sdcard/roms"
#define SAVES_DIR   "/sdcard/saves"

// Audio config
#define AUDIO_SAMPLES   1024
#define AUDIO_SAMPLE_RATE 32768

// ── Global state ──────────────────────────────────────────────────────────────

static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb_pax               = {0};
static QueueHandle_t              input_event_queue    = NULL;

// mGBA core
static struct mCore *core = NULL;
static color_t      *gba_fb = NULL;  // mGBA renders into this (32-bit XBGR)

// Task handles
static TaskHandle_t      emulator_task_handle = NULL;

// Input state
static volatile uint32_t gba_keys = 0;

// FPS tracking
static volatile int fps_val = 0;

// ── Helpers ───────────────────────────────────────────────────────────────────

static void restart_to_launcher(void) {
    bootloader_common_update_rtc_retain_mem(NULL, true);
    esp_restart();
}

static void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb_pax));
}

// Find first .gba ROM in ROMS_DIR
static const char *find_rom(void) {
    static char rom_path[320];
    DIR *d = opendir(ROMS_DIR);
    if (!d) return NULL;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len > 4 && strcasecmp(ent->d_name + len - 4, ".gba") == 0) {
            snprintf(rom_path, sizeof(rom_path), "%s/%s", ROMS_DIR, ent->d_name);
            closedir(d);
            return rom_path;
        }
    }
    closedir(d);
    return NULL;
}

// ── Scale GBA framebuffer into PAX buffer and blit ───────────────────────────

// Following HowBoyMatsu's full-screen fill approach:
// GBA X (240) → 800 physical rows (3.33x: every 3rd gx gets 4 rows, rest get 3)
// GBA Y (160, reversed) → 480 physical columns (3x exact)

static void scale_and_blit(void) {
    uint8_t *dst = (uint8_t *)pax_buf_get_pixels(&fb_pax);
    const int bpp = 3;
    const int stride = PHYS_W * bpp;  // 480 * 3 = 1440 bytes per physical row

    uint8_t scaled_row[PHYS_W * 3];
    int row = 0;

    for (int gx = 0; gx < GBA_WIDTH; gx++) {
        // Build scaled row: GBA Y reversed → 480 physical columns (3x exact)
        uint8_t *rp = scaled_row;

        for (int gy = GBA_HEIGHT - 1; gy >= 0; gy--) {
            color_t c = gba_fb[gy * GBA_WIDTH + gx];
            uint8_t r = c & 0xFF;
            uint8_t g = (c >> 8) & 0xFF;
            uint8_t b = (c >> 16) & 0xFF;
            // 3x column scale
            rp[0] = r; rp[1] = g; rp[2] = b;
            rp[3] = r; rp[4] = g; rp[5] = b;
            rp[6] = r; rp[7] = g; rp[8] = b;
            rp += 9;
        }

        // Row scale: 3 or 4 rows (800/240 = 3.33x, every 3rd gx gets 4)
        int row_count = (gx % 3 == 0) ? 4 : 3;
        uint8_t *row_dst = dst + row * stride;
        for (int rep = 0; rep < row_count; rep++) {
            memcpy(row_dst, scaled_row, stride);
            row_dst += stride;
        }
        row += row_count;
    }

    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb_pax));
}

// ── Input handling ────────────────────────────────────────────────────────────

// GBA key bits (from mgba/gba/input.h or GBA hardware spec)
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

static void poll_input(void) {
    // Handle navigation events (d-pad + F1) from the event queue
    bsp_input_event_t ev;
    while (xQueueReceive(input_event_queue, &ev, 0) == pdTRUE) {
        if (ev.type == INPUT_EVENT_TYPE_NAVIGATION) {
            uint32_t bit = 0;
            bool handled = false;
            switch (ev.args_navigation.key) {
                case BSP_INPUT_NAVIGATION_KEY_UP:    bit = GBA_KEY_UP;    handled = true; break;
                case BSP_INPUT_NAVIGATION_KEY_DOWN:  bit = GBA_KEY_DOWN;  handled = true; break;
                case BSP_INPUT_NAVIGATION_KEY_LEFT:  bit = GBA_KEY_LEFT;  handled = true; break;
                case BSP_INPUT_NAVIGATION_KEY_RIGHT: bit = GBA_KEY_RIGHT; handled = true; break;
                case BSP_INPUT_NAVIGATION_KEY_F1:
                    if (ev.args_navigation.state) restart_to_launcher();
                    break;
                default: break;
            }
            if (handled) {
                if (ev.args_navigation.state)
                    gba_keys |= bit;
                else
                    gba_keys &= ~bit;
            }
        }
    }

    // Poll keyboard keys by scancode — accurate press/release, no repeat flicker
    bool st;
    bsp_input_read_scancode(BSP_INPUT_SCANCODE_X,     &st); if (st) gba_keys |= GBA_KEY_A;      else gba_keys &= ~GBA_KEY_A;
    bsp_input_read_scancode(BSP_INPUT_SCANCODE_Z,     &st); if (st) gba_keys |= GBA_KEY_B;      else gba_keys &= ~GBA_KEY_B;
    bsp_input_read_scancode(BSP_INPUT_SCANCODE_Q,     &st); if (st) gba_keys |= GBA_KEY_L;      else gba_keys &= ~GBA_KEY_L;
    bsp_input_read_scancode(BSP_INPUT_SCANCODE_E,     &st); if (st) gba_keys |= GBA_KEY_R;      else gba_keys &= ~GBA_KEY_R;
    bsp_input_read_scancode(BSP_INPUT_SCANCODE_ENTER, &st); if (st) gba_keys |= GBA_KEY_START;  else gba_keys &= ~GBA_KEY_START;
    bsp_input_read_scancode(BSP_INPUT_SCANCODE_SPACE, &st); if (st) gba_keys |= GBA_KEY_SELECT; else gba_keys &= ~GBA_KEY_SELECT;
}

// ── mGBA logging callback ─────────────────────────────────────────────────────

static void mgba_log(struct mLogger *logger, int category, enum mLogLevel level, const char *format, va_list args) {
    (void)logger; (void)category;
    if (level & (mLOG_ERROR | mLOG_FATAL)) {
        esp_log_writev(ESP_LOG_ERROR, "mGBA", format, args);
    }
}

static struct mLogger mgba_logger = { .log = mgba_log };

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
            pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 40, "Press F1 to return");
            blit();
            goto wait_exit;
        }
        ESP_LOGI(TAG, "SD card mounted");
        sd_mounted = 1;
    }
sd_ready:;

    // ── Find ROM ─────────────────────────────────────────────────────────
    const char *rom_path = find_rom();
    if (!rom_path) {
        pax_background(&fb_pax, 0xFF000000);
        pax_draw_text(&fb_pax, 0xFFFF0000, pax_font_sky_mono, 16, 10, 10, "No GBA ROMs found!");
        pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 40, "Place .gba files in /sdcard/roms/");
        pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 70, "Press F1 to return");
        blit();
        goto wait_exit;
    }

    ESP_LOGI(TAG, "Loading ROM: %s", rom_path);
    pax_background(&fb_pax, 0xFF000000);
    pax_draw_text(&fb_pax, 0xFFFFFF00, pax_font_sky_mono, 14, 10, 10, "Loading ROM...");
    blit();

    // ── Initialize mGBA core ─────────────────────────────────────────────
    mLogSetDefaultLogger(&mgba_logger);

    core = GBACoreCreate();
    if (!core) {
        ESP_LOGE(TAG, "Failed to create GBA core");
        goto wait_exit;
    }
    core->init(core);
    mCoreInitConfig(core, NULL);

    // Allocate GBA framebuffer from internal RAM (150KB — saves PSRAM for ROM)
    gba_fb = heap_caps_malloc(GBA_WIDTH * GBA_HEIGHT * sizeof(color_t), MALLOC_CAP_INTERNAL);
    if (!gba_fb) {
        ESP_LOGE(TAG, "Failed to allocate GBA framebuffer");
        goto wait_exit;
    }
    memset(gba_fb, 0, GBA_WIDTH * GBA_HEIGHT * sizeof(color_t));
    core->setVideoBuffer(core, gba_fb, GBA_WIDTH);

    // Free PAX buffer to maximize PSRAM for ROM mapping (mGBA maps up to 32MB)
    ESP_LOGI(TAG, "Free PSRAM before ROM load: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    pax_buf_destroy(&fb_pax);

    // Load ROM directly from SD card via VFile
    struct VFile *vf = VFileFOpen(rom_path, "rb");
    if (!vf || !core->loadROM(core, vf)) {
        ESP_LOGE(TAG, "mGBA failed to load ROM (free PSRAM: %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        // Re-create PAX buffer for error display
        pax_buf_init(&fb_pax, NULL, display_h_res, display_v_res, PAX_BUF_24_888RGB);
        pax_buf_reversed(&fb_pax, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
        pax_buf_set_orientation(&fb_pax, PAX_O_ROT_CW);
        pax_background(&fb_pax, 0xFF000000);
        pax_draw_text(&fb_pax, 0xFFFF0000, pax_font_sky_mono, 16, 10, 10, "Failed to load ROM!");
        pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 40, "ROM may be too large for PSRAM");
        blit();
        goto wait_exit;
    }
    ESP_LOGI(TAG, "ROM loaded via VFile: %s (free PSRAM: %u)", rom_path,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Re-create PAX buffer for display output
    pax_buf_init(&fb_pax, NULL, display_h_res, display_v_res, PAX_BUF_24_888RGB);
    pax_buf_reversed(&fb_pax, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
    pax_buf_set_orientation(&fb_pax, PAX_O_ROT_CW);

    // Set up save file
    mkdir(SAVES_DIR, 0777);
    {
        char save_path[320];
        const char *base = strrchr(rom_path, '/');
        base = base ? base + 1 : rom_path;
        snprintf(save_path, sizeof(save_path), "%s/%s", SAVES_DIR, base);
        char *dot = strrchr(save_path, '.');
        if (dot) strcpy(dot, ".sav");
        struct VFile *save_vf = VFileOpen(save_path, O_CREAT | O_RDWR);
        if (save_vf) {
            core->loadSave(core, save_vf);
            ESP_LOGI(TAG, "Save file: %s", save_path);
        }
    }

    core->reset(core);
    ESP_LOGI(TAG, "mGBA core initialized — starting emulation");

    // Clear display
    pax_background(&fb_pax, 0xFF000000);
    blit();

    // ── Main emulation loop ──────────────────────────────────────────────
    {
        int64_t frame_start = esp_timer_get_time();
        int frame_count = 0;

        const int FRAME_SKIP = 1;  // run N extra frames without rendering

        while (1) {
            poll_input();
            core->setKeys(core, gba_keys);

            // Run skipped frames (no render)
            for (int i = 0; i < FRAME_SKIP; i++) {
                core->runFrame(core);
            }
            // Run + render frame
            core->runFrame(core);
            scale_and_blit();

            // FPS counter (counts emulated frames, not rendered)
            frame_count += FRAME_SKIP + 1;
            int64_t now = esp_timer_get_time();
            if (now - frame_start >= 1000000) {
                fps_val = frame_count;
                frame_count = 0;
                frame_start = now;
                ESP_LOGI(TAG, "FPS: %d (skip %d)", fps_val, FRAME_SKIP);
            }
        }
    }

wait_exit:;
    // Wait for F1 to return to launcher
    {
        bsp_input_event_t ev;
        while (1) {
            if (xQueueReceive(input_event_queue, &ev, portMAX_DELAY) == pdTRUE) {
                if (ev.type == INPUT_EVENT_TYPE_NAVIGATION &&
                    ev.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1 &&
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

    // Initialize BSP with RGB888 display (PAX needs this)
    const bsp_configuration_t bsp_configuration = {
        .display = {
            .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
            .num_fbs = 1,
        },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) { ESP_LOGE(TAG, "BSP init failed: %d", res); return; }

    res = bsp_display_get_parameters(&display_h_res, &display_v_res,
                                      &display_color_format, &display_data_endian);
    if (res != ESP_OK) { ESP_LOGE(TAG, "Display params failed: %d", res); return; }

    // PAX framebuffer setup with rotation
    pax_buf_type_t format = PAX_BUF_24_888RGB;
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

    // Splash screen
    pax_background(&fb_pax, 0xFF000000);
    pax_draw_text(&fb_pax, 0xFF00FF00, pax_font_sky_mono, 24, 10, 10,  "HowBoyAdvance");
    pax_draw_text(&fb_pax, 0xFFFFFFFF, pax_font_sky_mono, 14, 10, 50,  "Game Boy Advance Emulator");
    pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 80,  "for Tanmatsu");
    pax_draw_text(&fb_pax, 0xFFFFFF00, pax_font_sky_mono, 12, 10, 120, "Loading...");
    blit();
    vTaskDelay(pdMS_TO_TICKS(500));

    xTaskCreatePinnedToCore(emulator_task, "emulator", 32768, NULL, 5, &emulator_task_handle, 1);

    // Main task can idle — emulator runs on Core 1, blit on Core 0
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
