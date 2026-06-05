This Project "HowBoyAdvance" is a Game Boy Advance Emulator for the handheld
Tanmatsu Console (ESP32-P4). Sister project to HowBoyMatsu (GBC emulator).
Uses gpSP core with RISC-V dynarec for GBA emulation.

## Architecture

- **main/main.c** — App entry, ROM selector, emulation loop, input, display, audio
- **main/rom_selector.c** — On-screen ROM file browser with scrolling
- **main/menu.c** — Save state menu + layout menu overlays (5x7 bitmap font)
- **main/config.h** — Shared constants (screen dims, menu geometry, save state size)
- **main/menu.h** — Menu state externs and draw function declarations
- **components/gpsp/** — gpSP emulator core with RISC-V dynarec
  - `riscv/riscv_codegen.h` — RISC-V machine code generation macros
  - `riscv/riscv_emit.h` — JIT block emission (ARM/Thumb → RISC-V)
  - `riscv/riscv_stub.S` — Register file layout, entry/exit asm stubs
  - `gpsp_esp.c` — ESP32-P4 integration (JIT MMU mapping, ROM load, save states)
  - `gpsp_memory_alloc.c` — Large arrays in PSRAM via EXT_RAM_BSS_ATTR
  - `gpsp_config.h` — JIT cache sizes, ROM buffer config
  - `sound.c` — Audio rendering at 65536 Hz into circular buffer
- **build.bat / upload.bat** — Windows build/upload helpers

## Key Facts

- Display: 800x480 MIPI DSI, RGB565, portrait 480x800 rotated 90 CW for landscape
- GBA: 240x160, PPA hardware scaled 3.3125x/3.0x + 270° rotation → 480x795, centered
- ROM path: /sdcard/roms/*.gba
- Save path: /sdcard/saves/*.sav
- Save states: /sdcard/saves/*.ss0 through .ss9 (416KB BSON each)
- Build: make build DEVICE=tanmatsu (or build.bat on Windows)
- Always delete build/tanmatsu/esp-idf/main/CMakeFiles/__idf_main.dir/main.c.obj before rebuild
- For gpsp-wide changes: delete build/tanmatsu/esp-idf/gpsp/CMakeFiles/__idf_gpsp.dir/
- Emulator task: 48KB stack, pinned to Core 1
- Blit task: Core 0, PPA scale + bsp_display_blit
- Audio task: Core 0, I2S write from double buffer at 65536 Hz
- Input: navigation keys via event queue, keyboard via scancode polling
- gpSP dynarec: JIT caches in PSRAM with exec MMU mapping
- JIT ROM cache: 1MB, RAM cache: 256KB (SMALL_TRANSLATION_CACHE)
- Save state buffer: allocated on-demand (416KB PSRAM) to save memory for ROM
- ROM loading: gpSP allocates 1MB blocks greedily, page-swaps from SD for large ROMs
- `-msmall-data-limit=0` global compile flag prevents .sbss overflow
- Sister project HowBoyMatsu (C:\Users\Howar\HowBoyMatsu) is the reference for Tanmatsu hardware compatibility
- Always update DEVLOG.md, README.md, and CLAUDE.md with every commit

## Current State (Session 4)

- Switched from mGBA interpreter to gpSP with RISC-V dynarec (~49-69 FPS)
- Full feature set: save states (F4), FPS overlay, fast forward (F6), soft reset (F1),
  layout switcher (F2), volume control, ROM selector, exit to launcher (ESC), autosave SRAM
- Audio output working (65536 Hz I2S) but has some crackling
- PSRAM optimized: on-demand save state buf, reduced JIT cache (1MB+256KB)
- Rewind (F5) attempted but removed — needs ~20MB PSRAM, doesn't fit with 16MB ROMs
- RTC "battery dry" fix attempted but removed — settimeofday pulls in too much libc BSS
