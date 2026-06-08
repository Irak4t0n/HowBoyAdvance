# HowBoyAdvance

A Game Boy Advance emulator for the [Tanmatsu](https://tanmatsu.badge.team/) handheld console.

Sister project to [HowBoyMatsu](https://github.com/Irak4t0n/HowBoyMatsu) (Game Boy Color emulator).

## Hardware

- **SoC**: ESP32-P4 (360MHz dual-core RISC-V)
- **RAM**: 32MB PSRAM
- **Flash**: 16MB
- **Display**: 800x480 MIPI DSI (portrait 480x800, rotated 90 CW for landscape)

## Emulator

Uses [gpSP](https://github.com/libretro/gpsp) with a custom RISC-V dynamic recompiler (dynarec) for GBA emulation.

- **RISC-V dynarec**: JIT compiles ARM/Thumb → native RISC-V for ~2x speedup over interpreter
- GBA screen (240x160) scaled to full 800x480 via PPA hardware scaler (3.3x/3.0x + rotation)
- Audio output at 65536 Hz via I2S to ES8156 codec with soft clipping
- Dual-core pipeline: Core 1 emulates, Core 0 PPA scales + blits
- ROM page-swapping from SD card for ROMs larger than available PSRAM cache
- Save files stored on SD card at `/sdcard/saves/`
- Save states stored at `/sdcard/saves/*.ss0` through `.ss9`

## Controls

### Default Layout
| GBA Button | Tanmatsu Key |
|------------|-------------|
| A          | A           |
| B          | D           |
| L          | Q           |
| R          | E           |
| Start      | Enter       |
| Select     | Space       |
| D-pad      | Arrow keys  |

### WASD Layout (F2 to switch)
| GBA Button | Tanmatsu Key |
|------------|-------------|
| A          | ; (semicolon) |
| B          | [ (bracket)   |
| D-pad      | WASD          |

### System Keys
| Function | Key |
|----------|-----|
| Soft reset | F1 |
| Layout switcher | F2 |
| Save state menu | F4 |
| Fast forward (OFF/5x/8x) | F6 |
| FPS overlay | ` (backtick) |
| Volume up | Volume Up |
| Volume down | Volume Down |
| Return to ROM selector | Backspace |
| Exit to launcher | ESC |

## Usage

1. Place `.gba` ROM files in `/sdcard/roms/` on the SD card
2. Launch HowBoyAdvance from the Tanmatsu app menu
3. Select a ROM from the on-screen file browser

## Build

```bash
# Prepare SDK (first time only)
make prepare

# Build
make build DEVICE=tanmatsu

# Upload (put Tanmatsu in badgelink mode first)
make install DEVICE=tanmatsu
```

Windows users can also use `build.bat` and `upload.bat` helpers.

## Architecture

- **main/main.c** - Application entry, ROM selector, emulation loop, input, display, audio
- **main/rom_selector.c** - On-screen ROM file browser
- **main/menu.c** - Save state and layout menu overlays (5x7 bitmap font)
- **main/config.h** - Shared constants (screen dimensions, menu layout)
- **components/gpsp/** - gpSP emulator core with RISC-V dynarec
  - `riscv/riscv_codegen.h` - RISC-V machine code generation
  - `riscv/riscv_emit.h` - JIT block emission (ARM/Thumb → RISC-V)
  - `riscv/riscv_stub.S` - Register file, entry/exit stubs
  - `gpsp_esp.c` - ESP32-P4 integration (JIT cache mapping, ROM loading)
  - `gpsp_memory_alloc.c` - Large arrays in PSRAM via EXT_RAM_BSS_ATTR

## Performance

~49-69 FPS on Pokemon Emerald with RISC-V dynarec.

## Known Issues

- 32MB ROMs work via page-swapping but may have brief hitches on cache misses

## License

gpSP is licensed under the GNU General Public License v2.0.
