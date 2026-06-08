#pragma once

#define ROMS_DIR  "/sdcard/roms"
#define SAVES_DIR "/sdcard/saves"
#define MAX_ROMS  256

extern char rom_list[MAX_ROMS][300];
extern int  rom_count;

void        scan_roms(void);
const char *rom_selector(void);
