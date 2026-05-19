# HowBoyAdvance Development Log

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
