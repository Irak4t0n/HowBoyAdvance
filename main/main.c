#include <stdio.h>
#include <string.h>
#include <stdarg.h>
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
#include "hal/lcd_types.h"
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
#include <mgba/core/serialize.h>
#include <mgba/core/blip_buf.h>
#include <mgba/gba/core.h>
#include <mgba/gba/interface.h>
#include <mgba-util/vfs.h>

// ── Constants ─────────────────────────────────────────────────────────────────

static char const TAG[] = "howboy";

#define GBA_WIDTH   240
#define GBA_HEIGHT  160
#define PHYS_W      480   // landscape width  (display is 800x480 portrait, rotated)
#define PHYS_H      800   // landscape height
#define SCALE       2     // 240*2=480, 160*2=320 — fits in 800x480
#define SCALED_W    (GBA_WIDTH  * SCALE)  // 480
#define SCALED_H    (GBA_HEIGHT * SCALE)  // 320
#define OFFSET_X    0                          // (800 - 480) / 2 ... but we center vertically in landscape
#define OFFSET_Y    ((PHYS_H - SCALED_H) / 2) // (800 - 320) / 2 = 240

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

// Display double-buffer (PAX format: landscape 800x480, written as portrait 480x800)
static uint8_t *render_buf_a = NULL;
static uint8_t *render_buf_b = NULL;
static int      active_buf   = 0;

// Task sync
static SemaphoreHandle_t sem_frame_ready  = NULL;
static SemaphoreHandle_t sem_frame_done   = NULL;
static SemaphoreHandle_t sem_emulator_done = NULL;
static TaskHandle_t      blit_task_handle  = NULL;
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

// Read ROM from SD using chunk loop (ftell can return 0 on FAT)
static uint8_t *load_rom_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    // Try ftell first
    fseek(f, 0, SEEK_END);
    size_t sz = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz == 0) {
        // ftell failed on FAT — read in chunks
        size_t cap = 256 * 1024;
        uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (!buf) { fclose(f); return NULL; }
        sz = 0;
        while (1) {
            if (sz >= cap) {
                cap *= 2;
                uint8_t *nb = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
                if (!nb) { free(buf); fclose(f); return NULL; }
                memcpy(nb, buf, sz);
                free(buf);
                buf = nb;
            }
            size_t r = fread(buf + sz, 1, cap - sz, f);
            if (r == 0) break;
            sz += r;
        }
        fclose(f);
        *out_size = sz;
        return buf;
    }

    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    fclose(f);
    *out_size = sz;
    return buf;
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

// ── Scale GBA framebuffer to display ──────────────────────────────────────────

// mGBA outputs 32-bit color_t (XBGR8 on little-endian: 0x00BBGGRR).
// PAX buffer is RGB888 (3 bytes per pixel, R-G-B order) in portrait layout.
// The display is physically 480w x 800h portrait, but we treat it as 800x480 landscape
// via PAX rotation. We write directly into the render buffer in portrait coords.
//
// Portrait mapping: pixel at landscape (lx, ly) maps to portrait (row, col):
//   row = lx,  col = (PHYS_H - 1) - ly   (for 90° CW rotation)

static void scale_gba_to_render_buf(void) {
    uint8_t *dst = active_buf ? render_buf_b : render_buf_a;
    const int bpp = 3;  // RGB888 = 3 bytes per pixel
    const int row_stride = PHYS_H * bpp;  // portrait: 800 pixels wide * 3 bytes

    // Clear the border regions (top/bottom bars in landscape = left/right in portrait)
    // Only need to do this once, but it's cheap enough
    // The OFFSET_Y pixels at top and bottom in landscape map to columns in portrait

    // Scale 2x: each GBA pixel becomes a 2x2 block
    for (int gy = 0; gy < GBA_HEIGHT; gy++) {
        for (int gx = 0; gx < GBA_WIDTH; gx++) {
            color_t c = gba_fb[gy * GBA_WIDTH + gx];
            // mGBA XBGR8: bits [7:0]=R, [15:8]=G, [23:16]=B
            uint8_t r = c & 0xFF;
            uint8_t g = (c >> 8) & 0xFF;
            uint8_t b = (c >> 16) & 0xFF;

            // Landscape coords of scaled pixel
            int lx0 = gx * SCALE;           // 0..478
            int ly0 = OFFSET_Y + gy * SCALE; // 240..559

            // Write 2x2 block
            for (int dy = 0; dy < SCALE; dy++) {
                int ly = ly0 + dy;
                // Portrait coords: row = lx, col = (PHYS_H-1) - ly
                int col = (PHYS_H - 1) - ly;
                for (int dx = 0; dx < SCALE; dx++) {
                    int lx = lx0 + dx;
                    int row = lx;
                    int off = row * row_stride + col * bpp;
                    dst[off + 0] = r;
                    dst[off + 1] = g;
                    dst[off + 2] = b;
                }
            }
        }
    }
}

// ── Blit task (Core 0) ───────────────────────────────────────────────────────

static void blit_task(void *arg) {
    while (1) {
        xSemaphoreTake(sem_frame_ready, portMAX_DELAY);

        uint8_t *buf = active_buf ? render_buf_b : render_buf_a;
        bsp_display_blit(0, 0, PHYS_W, PHYS_H, buf);

        xSemaphoreGive(sem_frame_done);
    }
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
    bsp_input_event_t ev;
    while (xQueueReceive(input_event_queue, &ev, 0) == pdTRUE) {
        uint32_t bit = 0;
        bool handled = false;

        if (ev.type == INPUT_EVENT_TYPE_NAVIGATION) {
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
        else if (ev.type == INPUT_EVENT_TYPE_KEYBOARD) {
            char c = ev.args_keyboard.ascii;
            bool pressed = (ev.args_keyboard.modifiers & 0x01) || (c != 0); // key event
            // For keyboard events, we get press only — track via scancode instead
            // Actually keyboard events come as characters, we need scancode for release detection
        }
        else if (ev.type == INPUT_EVENT_TYPE_SCANCODE) {
            uint32_t sc = ev.args_scancode.scancode;
            bool pressed = (ev.args_scancode.state != 0);
            // Tanmatsu keyboard scancodes for QWERTY keys
            // x=A, z=B, q=L, e=R, Enter=Start, Space=Select
            switch (sc) {
                case 0x1B: bit = GBA_KEY_A;      break; // x
                case 0x1A: bit = GBA_KEY_B;      break; // z
                case 0x14: bit = GBA_KEY_L;      break; // q
                case 0x08: bit = GBA_KEY_R;      break; // e
                case 0x28: bit = GBA_KEY_START;  break; // Enter
                case 0x2C: bit = GBA_KEY_SELECT; break; // Space
                default: break;
            }
            if (bit) {
                if (pressed)
                    gba_keys |= bit;
                else
                    gba_keys &= ~bit;
            }
        }
    }
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

    // ── Load ROM into PSRAM ──────────────────────────────────────────────
    size_t rom_size = 0;
    uint8_t *rom_data = load_rom_file(rom_path, &rom_size);
    if (!rom_data || rom_size == 0) {
        ESP_LOGE(TAG, "Failed to load ROM (%u bytes)", (unsigned)rom_size);
        pax_background(&fb_pax, 0xFF000000);
        pax_draw_text(&fb_pax, 0xFFFF0000, pax_font_sky_mono, 16, 10, 10, "Failed to load ROM!");
        blit();
        goto wait_exit;
    }
    ESP_LOGI(TAG, "ROM loaded: %u bytes", (unsigned)rom_size);

    // ── Initialize mGBA core ─────────────────────────────────────────────
    mLogSetDefaultLogger(&mgba_logger);

    core = GBACoreCreate();
    if (!core) {
        ESP_LOGE(TAG, "Failed to create GBA core");
        goto wait_exit;
    }
    core->init(core);

    // Allocate GBA framebuffer in PSRAM (240x160 x 4 bytes)
    gba_fb = heap_caps_malloc(GBA_WIDTH * GBA_HEIGHT * sizeof(color_t), MALLOC_CAP_SPIRAM);
    if (!gba_fb) {
        ESP_LOGE(TAG, "Failed to allocate GBA framebuffer");
        goto wait_exit;
    }
    memset(gba_fb, 0, GBA_WIDTH * GBA_HEIGHT * sizeof(color_t));
    core->setVideoBuffer(core, gba_fb, GBA_WIDTH);

    // Load ROM via VFile memory wrapper (VFileMemChunk for const/read-only)
    struct VFile *vf = VFileMemChunk(rom_data, rom_size);
    if (!vf || !core->loadROM(core, vf)) {
        ESP_LOGE(TAG, "mGBA failed to load ROM");
        goto wait_exit;
    }

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

    // Clear render buffers (black borders)
    memset(render_buf_a, 0, PHYS_W * PHYS_H * 3);
    memset(render_buf_b, 0, PHYS_W * PHYS_H * 3);

    // ── Main emulation loop ──────────────────────────────────────────────
    {
        int64_t frame_start = esp_timer_get_time();
        int frame_count = 0;

        while (1) {
            poll_input();
            core->setKeys(core, gba_keys);
            core->runFrame(core);

            // Wait for previous blit to finish, then scale into the back buffer
            xSemaphoreTake(sem_frame_done, portMAX_DELAY);
            scale_gba_to_render_buf();
            active_buf ^= 1;
            xSemaphoreGive(sem_frame_ready);

            // FPS counter
            frame_count++;
            int64_t now = esp_timer_get_time();
            if (now - frame_start >= 1000000) {
                fps_val = frame_count;
                frame_count = 0;
                frame_start = now;
                ESP_LOGI(TAG, "FPS: %d", fps_val);
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
    pax_buf_reversed(&fb_pax, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
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

    // Allocate render double-buffers in PSRAM (portrait: 480x800x3 = 1.152 MB each)
    size_t buf_sz = PHYS_W * PHYS_H * 3;  // RGB888
    render_buf_a = (uint8_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM);
    render_buf_b = (uint8_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM);
    if (!render_buf_a || !render_buf_b) {
        ESP_LOGE(TAG, "Failed to allocate render buffers");
        return;
    }
    memset(render_buf_a, 0, buf_sz);
    memset(render_buf_b, 0, buf_sz);

    sem_frame_ready = xSemaphoreCreateBinary();
    sem_frame_done  = xSemaphoreCreateBinary();
    xSemaphoreGive(sem_frame_done);

    xTaskCreatePinnedToCore(blit_task, "blit", 8192, NULL, 5, &blit_task_handle, 0);
    xTaskCreatePinnedToCore(emulator_task, "emulator", 32768, NULL, 5, &emulator_task_handle, 1);

    // Main task can idle — emulator runs on Core 1, blit on Core 0
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
