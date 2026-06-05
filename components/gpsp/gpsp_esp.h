/* gpSP ESP32-P4 platform interface */
#ifndef GPSP_ESP_H
#define GPSP_ESP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize gpSP subsystems (call once before loading ROM) */
bool gpsp_init(void);

/* Load ROM from file path. Returns 0 on success. */
int gpsp_load_rom(const char *rom_path);

/* Run one frame of emulation. Returns pointer to 240x160 RGB565 framebuffer. */
uint16_t *gpsp_run_frame(void);

/* Set button state. Uses GBA button bits:
 * bit 0=A, 1=B, 2=Select, 3=Start, 4=Right, 5=Left, 6=Up, 7=Down, 8=R, 9=L */
void gpsp_set_buttons(uint16_t buttons);

/* Get audio samples. Returns number of stereo frames written. */
unsigned gpsp_get_audio(int16_t *out, unsigned max_frames);

/* Load/save battery-backed save */
int gpsp_load_save(const char *path);
int gpsp_write_save(const char *path);

/* Save/load state (buffer must be >= 416KB) */
void gpsp_save_state_buf(void *buf);
bool gpsp_load_state_buf(const void *buf);

/* Soft reset: reinit CPU/memory/sound, flush dynarec */
void gpsp_reset(void);

/* Cleanup */
void gpsp_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
