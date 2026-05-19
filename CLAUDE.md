This Project "HowBoyAdvance" is a Game Boy Advance Emulator for the handheld
Tanmatsu Console (ESP32-P4). Sister project to HowBoyMatsu (GBC emulator).
Uses mGBA 0.10.4 core for GBA emulation.

## Architecture

- **main/main.c** — App entry point, ROM loading, emulation loop, input, display
- **components/mgba/** — mGBA 0.10.4 core, stripped to MINIMAL_CORE=2
  - `util/vfs/vfs-file.c` — VFile stdio backend; ESP path uses staged DMA reads
  - `util/memory.c` — anonymousMemoryMap routes >=64KB to PSRAM
  - `gb/audio.c` — GB PSG audio (shared by GBA audio subsystem)
  - `CMakeLists.txt` — ESP-IDF component registration, compile defs, sources
- **build.bat / upload.bat** — Windows build/upload helpers

## Key Facts

- Display: 800x480 MIPI DSI, RGB565, portrait 480x800 rotated 90 CW for landscape
- GBA: 240x160, scaled to full 800x480 (3.33x rows via 4-3-3 pattern, 3x columns)
- ROM path: /sdcard/roms/*.gba
- Save path: /sdcard/saves/*.sav
- Build: make build DEVICE=tanmatsu (or build.bat on Windows)
- Always delete build/tanmatsu/esp-idf/main/libmain.a before rebuild to force recompile
- bsp_display_blit() works with raw RGB565 render buffers (not just PAX)
- ROM must go into PSRAM via heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
- Emulator task: 32KB stack, pinned to Core 1
- mCoreInitConfig(core, NULL) must be called after core->init() before core->reset()
- ROM loading uses staged reads through 128KB internal DMA buffer (~7 MB/s)
- mGBA built-in frameskip=1: renderer skipped on alternate frames (CPU still runs)
- Idle loop detection: mCoreConfigSetValue "idleOptimization" = "detect"
- Dual-core pipeline: Core 1 emulates, Core 0 scales gba_fb + blits to display
- PSRAM bus contention: Core 0 writing render_buf slows Core 1's PSRAM reads
- RGB565 reduces PSRAM bandwidth 33% vs RGB888, directly improves FPS
- Audio channels disabled (core->enableAudioChannel) until audio output implemented
- Sister project HowBoyMatsu (C:\Users\Howar\HowBoyMatsu) is the reference for Tanmatsu hardware compatibility
- Always update DEVLOG.md, README.md, and CLAUDE.md with every commit
