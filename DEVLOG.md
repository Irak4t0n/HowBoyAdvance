# HowBoyAdvance Development Log

## 2026-06-05: gpSP Dynarec + Full Feature Set

### Core Switch: mGBA → gpSP
- Replaced mGBA 0.10.4 interpreter with gpSP emulator core
- Implemented RISC-V dynamic recompiler (dynarec) for ESP32-P4
- JIT compiles ARM/Thumb GBA code → native RISC-V machine code
- Performance: 49-69 FPS on Pokemon Emerald (vs 28-44 with mGBA interpreter)

### Dynarec Implementation (RISC-V)
- Custom codegen in `riscv/riscv_codegen.h`, `riscv/riscv_emit.h`, `riscv/riscv_stub.S`
- JIT caches allocated in PSRAM, mapped executable via `esp_mmu_map`
- Register file (reg[64], spsr[6], reg_mode[7][7]) in contiguous `.ext_ram.bss.regfile` section
- Critical fix: `reg[REG_PC]` must be set before C calls in block prologues, load stubs, and store stubs
- Block prologue uses LUI+ADDI (not single ADDI) to avoid PSRAM corruption flipping opcode bits

### Display Pipeline
- PPA hardware scaler: 3.3125x/3.0x + 270° rotation → 480x795 centered in 480x800
- Single-buffered render_buf in PSRAM, overlays drawn after PPA scale

### Audio
- gpSP generates at 65536 Hz (GBA_SOUND_FREQUENCY = 64*1024)
- I2S configured to 65536 Hz via `bsp_audio_set_rate()`
- Double-buffered audio submission with blocking semaphore (30ms timeout)
- Fixed-size frame submission (1097 samples/frame) with silence padding
- Still has some crackling — likely DMA underruns at sub-60fps

### Emulator Features (matching HowBoyMatsu)
- **ROM selector**: On-screen file browser at `/sdcard/roms/`, supports up to 256 ROMs (alphabetically sorted)
- **Save states** (F4): 10 slots, 416KB BSON format, on-demand buffer allocation
- **FPS overlay** (backtick): Red digit overlay in corner
- **Fast forward** (F6): Cycles OFF → 5x → 8x
- **Soft reset** (F1): Resets GBA CPU + flushes dynarec caches
- **Layout switcher** (F2): Default (A/D=A/B) and WASD (;/[=A/B) layouts
- **Volume control**: Volume Up/Down keys
- **Return to ROM selector**: Backspace
- **Exit to launcher**: ESC (with SRAM autosave)
- **SRAM autosave**: Every ~5 minutes

### PSRAM Optimization
- Save state buffer: pre-allocated → on-demand (saves 416KB)
- JIT ROM cache: 2MB → 1MB (saves 1MB)
- JIT RAM cache: 384KB → 256KB (saves 128KB)
- Total: ~1.5MB freed for ROM cache, enabling larger ROM hacks
- `-msmall-data-limit=0` global flag prevents .sbss section overflow

### Features Attempted but Deferred
- **Rewind (F5)**: Needs ~20MB PSRAM (40 × 493KB snapshots), doesn't fit with 16MB ROMs
- **RTC fix**: `settimeofday()` pulls in libc timezone BSS, worsening linker overflow
- **Scale modes (F3)**: PPA hardware scaler is fundamentally different from GBC software scaling

### Build System
- Partition: 16M_noota.csv (4MB app, 8MB appfs, 4MB locfd)
- gpSP compiled with `-Oz -fno-exceptions -fno-rtti -msmall-data-limit=0`
- `-nostdlib++` linker flag prevents libstdc++ BSS overflow
- `-Wl,--no-check-sections` for irom/drom VMA overlap

## 2026-06-08: Audio Quality Fix + Launcher Icon

### Audio Improvements
- **Replaced hard clipper with two-segment cubic soft clipper** in `sound.c`
  - Segment 1 (|s| ≤ 4096): cubic y=(3x-x³)/2 with ~9x gain, output ±24576
  - Segment 2 (4096 < |s| ≤ 6144): Hermite extension to ±32767
  - Covers full GBA audio range (~±5900 worst case) without hard clipping
- **Recreated I2S with larger DMA buffers**: 8×550 frames (~67ms) vs BSP default (~22ms)
  - Eliminates DMA underrun crackling during brief frame drops
- **Fade-out before silence padding**: 32-frame linear fade prevents click artifacts
- **Digital volume scaling**: output × 3/4 (-2.5dB) for comfortable max volume at 100%
- **Removed dynamic sample rate control** (caused i2s_channel_reconfig errors)

### Launcher Icon
- `make_icon.py` — Generates 16x16, 32x32, 64x64 pixel art GBA icons (no dependencies)
- `metadata.json` — App store metadata with icon references
- GBA-shaped design: indigo body, green screen, shoulder buttons, d-pad, A/B buttons
- Published to app-repository as `com.irak4t0n.howboyadvance`

### Files Changed
- `components/gpsp/sound.c` — soft clipper + 75% volume scaling
- `main/main.c` — I2S recreation, fade-out padding, removed set_volume helper
- `make_icon.py` — new file, icon generator
- `metadata.json` — new file, app store metadata
- `icon-*.png` — new files, launcher icons

## 2026-05-18: Performance Optimization Pass

### Changes
- **RGB565 display** — Switched from RGB888 (3 bytes/pixel) to RGB565 (2 bytes/pixel). GBA outputs 15-bit color natively, so no visual quality loss. Reduces PSRAM write bandwidth by 33%.
- **mGBA built-in frameskip** — Set `core->opts.frameskip = 1` so mGBA's internal frameskip counter skips the software renderer (drawScanline + finishFrame) on alternate frames. CPU still runs all frames. Much more efficient than calling runFrame() twice externally.
- **Idle loop detection** — Enabled via `mCoreConfigSetValue("idleOptimization", "detect")`. mGBA auto-detects CPU idle loops (e.g., waiting for VBlank) and fast-forwards them. Huge impact on simple scenes (title screens, menus, standing still).
- **Dual-core scale+blit pipeline** — Core 1 emulates, Core 0 scales gba_fb into RGB565 render buffer and blits to display. Scale runs during the skip frame (which doesn't touch gba_fb), overlapping computation.
- **Disabled audio channels** — `core->enableAudioChannel()` disables all 6 channels (4 PSG + 2 DMA) to save CPU in blip_buf synthesis. Will re-enable when audio output is implemented.
- **mGBA compiled with -O2** instead of -Os for ~13% speed improvement.

### Performance Results (Pokemon Emerald)
| Scene | Emulated FPS | Displayed FPS |
|-------|-------------|---------------|
| Menus / standing still | 76 | 38 |
| Complex gameplay | 30-44 | 15-22 |
| Heavy transitions | 22 | 11 |

### Key Findings
- **ARM interpreter is the bottleneck** — runFrame takes 8ms (simple) to 43ms (complex) at 360MHz. No easy fix without JIT/dynarec.
- **PSRAM bus contention** — Core 0 writing render_buf to PSRAM slows Core 1's PSRAM reads (ROM, VRAM, EWRAM). RGB565 mitigated this by reducing write volume.
- **Memory threshold at 64KB** — Allocations >= 64KB go to PSRAM. Higher thresholds (100KB, 128KB) consumed internal RAM needed for the 128KB DMA staging buffer, regressing ROM load from 2.3s to 12s.
- **VRAM in internal RAM didn't help** — Tested routing VRAM (96KB) to internal RAM; no FPS improvement. ESP32-P4's cache is effective enough that PSRAM access for VRAM isn't the bottleneck.
