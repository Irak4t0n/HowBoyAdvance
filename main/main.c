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
#define LAND_W      PHYS_H  // 800
#define LAND_H      PHYS_W  // 480

#define ROMS_DIR    "/sdcard/roms"
#define SAVES_DIR   "/sdcard/saves"

// ── Global state ──────────────────────────────────────────────────────────────

static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb_pax               = {0};
static QueueHandle_t              input_event_queue    = NULL;

// mGBA core
static struct mCore *core = NULL;
static color_t      *gba_fb = NULL;

// Task handles
static TaskHandle_t      emulator_task_handle = NULL;

// Input state
static volatile uint32_t gba_keys = 0;

// FPS tracking
static volatile int fps_val = 0;

// Forward declaration (defined further below)
static void scale_frame(uint16_t *dst);

// Scale+blit pipeline: Core 0 scales gba_fb and blits while Core 1 emulates
static uint8_t          *render_buf[2] = {NULL, NULL};
static SemaphoreHandle_t sem_frame_ready = NULL;  // Core 1 → Core 0: gba_fb has new frame
static SemaphoreHandle_t sem_scale_done  = NULL;  // Core 0 → Core 1: done reading gba_fb

// ── Scale+blit task (Core 0) ─────────────────────────────────────────────────
// Scales gba_fb into render_buf, then blits to display.
// Runs in parallel with Core 1's skip frame (which doesn't touch gba_fb).

static void blit_task(void *arg) {
    int cur_buf = 0;
    while (1) {
        xSemaphoreTake(sem_frame_ready, portMAX_DELAY);
        // Scale gba_fb (internal RAM) into render_buf (PSRAM, RGB565)
        scale_frame((uint16_t *)render_buf[cur_buf]);
        // Signal Core 1: safe to write gba_fb again (next render frame)
        xSemaphoreGive(sem_scale_done);
        // Blit to display (overlaps with Core 1's next frames)
        bsp_display_blit(0, 0, display_h_res, display_v_res, render_buf[cur_buf]);
        cur_buf ^= 1;
    }
}

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

// ── Scale GBA framebuffer (RGB8888) into render buffer (RGB565) ───────────────

// GBA X (240) → 800 physical rows (3.33x: every 3rd gx gets 4 rows, rest get 3)
// GBA Y (160, reversed) → 480 physical columns (3x exact)
// Output is RGB565: 33% less PSRAM bandwidth than RGB888

static void scale_frame(uint16_t *dst) {
    const int stride = PHYS_W;  // 480 pixels per physical row

    uint16_t scaled_row[PHYS_W] __attribute__((aligned(4)));
    int row = 0;

    for (int gx = 0; gx < GBA_WIDTH; gx++) {
        // Build scaled row: each GBA pixel → 3 physical pixels (3x vertical scale)
        // Read source column bottom-to-top for 90° CW rotation
        uint16_t *rp = scaled_row;
        const color_t *src_col = gba_fb + gx;

        for (int gy = GBA_HEIGHT - 1; gy >= 0; gy--) {
            uint32_t c = src_col[gy * GBA_WIDTH];
            // Convert RGB888 → RGB565: R[7:3] G[7:2] B[7:3]
            uint16_t px = ((c & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c >> 19) & 0x1F);
            // Write 3 copies (3x column scaling)
            rp[0] = px;
            rp[1] = px;
            rp[2] = px;
            rp += 3;
        }

        // 4-3-3 row duplication pattern (240 → 800)
        int row_count = (gx % 3 == 0) ? 4 : 3;
        uint16_t *row_dst = dst + row * stride;
        memcpy(row_dst, scaled_row, stride * sizeof(uint16_t));
        for (int rep = 1; rep < row_count; rep++) {
            memcpy(row_dst + rep * stride, row_dst, stride * sizeof(uint16_t));
        }
        row += row_count;
    }
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

static void poll_input(void) {
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

    // Minimize audio processing overhead (we don't output audio yet)
    core->setAudioBufferSize(core, 4096);
    // Disable all 6 audio channels (4 PSG + 2 DMA)
    for (int ch = 0; ch < 6; ch++) {
        core->enableAudioChannel(core, ch, false);
    }

    // Allocate GBA framebuffer from internal RAM (150KB — saves PSRAM for ROM)
    gba_fb = heap_caps_malloc(GBA_WIDTH * GBA_HEIGHT * sizeof(color_t), MALLOC_CAP_INTERNAL);
    if (!gba_fb) {
        ESP_LOGE(TAG, "Failed to allocate GBA framebuffer");
        goto wait_exit;
    }
    memset(gba_fb, 0, GBA_WIDTH * GBA_HEIGHT * sizeof(color_t));
    core->setVideoBuffer(core, gba_fb, GBA_WIDTH);

    // Free PAX buffer to maximize PSRAM for ROM mapping
    pax_buf_destroy(&fb_pax);

    // Load ROM
    struct VFile *vf = VFileFOpen(rom_path, "rb");
    if (!vf || !core->loadROM(core, vf)) {
        ESP_LOGE(TAG, "mGBA failed to load ROM (free PSRAM: %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        pax_buf_init(&fb_pax, NULL, display_h_res, display_v_res, PAX_BUF_16_565RGB);
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

    // Re-create PAX buffer for error messages only (not used for game rendering)
    pax_buf_init(&fb_pax, NULL, display_h_res, display_v_res, PAX_BUF_16_565RGB);
    pax_buf_reversed(&fb_pax, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
    pax_buf_set_orientation(&fb_pax, PAX_O_ROT_CW);

    // Allocate double render buffers in PSRAM for blit task
    {
        size_t fb_size = PHYS_W * PHYS_H * 2;  // RGB565: 2 bytes per pixel
        render_buf[0] = heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
        render_buf[1] = heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
        if (!render_buf[0] || !render_buf[1]) {
            ESP_LOGE(TAG, "Failed to allocate render buffers");
            goto wait_exit;
        }
        memset(render_buf[0], 0, fb_size);
        memset(render_buf[1], 0, fb_size);
    }

    // Start blit task on Core 0
    sem_frame_ready = xSemaphoreCreateBinary();
    sem_scale_done  = xSemaphoreCreateBinary();
    xSemaphoreGive(sem_scale_done);  // Allow first render frame to proceed
    xTaskCreatePinnedToCore(blit_task, "blit", 4096, NULL, 6, NULL, 0);

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

    // Use mGBA's built-in frameskip: renderer is skipped for N frames,
    // then renders 1 frame. Skipped frames bypass drawScanline entirely.
    core->opts.frameskip = 1;

    // Enable idle loop detection: mGBA auto-detects when the CPU is spinning
    // in a tight loop waiting for an interrupt and fast-forwards those cycles.
    // Critical for Pokemon and other games that idle-wait for VBlank.
    mCoreConfigSetValue(&core->config, "idleOptimization", "detect");

    core->reset(core);
    ESP_LOGI(TAG, "mGBA core initialized — starting emulation");

    // ── Main emulation loop ──────────────────────────────────────────────
    // mGBA frameskip=1: render, skip, render, skip... (counter starts at 0)
    // Core 1: render frame → signal Core 0 → skip frame (overlaps with scale)
    // Core 0: scale gba_fb → signal done → blit to display
    {
        int64_t frame_start = esp_timer_get_time();
        int frame_count = 0;

        while (1) {
            poll_input();
            core->setKeys(core, gba_keys);

            // Wait for Core 0 to finish reading gba_fb from previous iteration
            xSemaphoreTake(sem_scale_done, portMAX_DELAY);

            // Rendered frame (mGBA counter=0: draws all scanlines + finishFrame)
            core->runFrame(core);

            // Signal Core 0 to scale gba_fb (safe: skip frame won't touch it)
            xSemaphoreGive(sem_frame_ready);

            // Skip frame (mGBA counter=1: CPU only, no renderer)
            // Core 0 scales gba_fb in parallel during this time
            core->runFrame(core);

            frame_count += 2;
            int64_t now = esp_timer_get_time();
            if (now - frame_start >= 1000000) {
                fps_val = frame_count;
                ESP_LOGI(TAG, "Emulated FPS: %d (displayed: %d)",
                         fps_val, fps_val / 2);
                frame_count = 0;
                frame_start = esp_timer_get_time();
            }
        }
    }

wait_exit:;
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

    // Splash screen
    pax_background(&fb_pax, 0xFF000000);
    pax_draw_text(&fb_pax, 0xFF00FF00, pax_font_sky_mono, 24, 10, 10,  "HowBoyAdvance");
    pax_draw_text(&fb_pax, 0xFFFFFFFF, pax_font_sky_mono, 14, 10, 50,  "Game Boy Advance Emulator");
    pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 80,  "for Tanmatsu");
    pax_draw_text(&fb_pax, 0xFFFFFF00, pax_font_sky_mono, 12, 10, 120, "Loading...");
    blit();
    vTaskDelay(pdMS_TO_TICKS(500));

    xTaskCreatePinnedToCore(emulator_task, "emulator", 32768, NULL, 5, &emulator_task_handle, 1);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
