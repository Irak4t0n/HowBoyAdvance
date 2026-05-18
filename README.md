# HowBoyAdvance

A Game Boy Advance emulator for the [Tanmatsu](https://tanmatsu.badge.team/) handheld console.

Sister project to [HowBoyMatsu](https://github.com/Irak4t0n/HowBoyMatsu) (Game Boy Color emulator).

## Hardware

- **SoC**: ESP32-P4 (400MHz dual-core RISC-V)
- **RAM**: 32MB PSRAM
- **Flash**: 16MB
- **Display**: 800x480 MIPI DSI (portrait 480x800, rotated 90 CW for landscape)

## Emulator

Uses [mGBA](https://mgba.io/) 0.10.4 core for GBA emulation.

- GBA screen (240x160) scaled to full 800x480 landscape display
  - 240 columns mapped to 800 rows (3.33x, pattern: 4-3-3 repeating)
  - 160 rows mapped to 480 columns (3x uniform)
- Software renderer, 32-bit color converted to RGB888 for PAX display
- Frame skipping (1 skip per rendered frame) for improved performance
- Optimized ROM loading: staged reads through internal DMA RAM (~7 MB/s)
- Save files stored on SD card at `/sdcard/saves/`

## Controls

| GBA Button | Tanmatsu Key |
|------------|-------------|
| A          | X           |
| B          | Z           |
| L          | Q           |
| R          | E           |
| Start      | Enter       |
| Select     | Space       |
| D-pad      | Arrow keys  |
| Return to launcher | F1 |

## Usage

1. Place `.gba` ROM files in `/sdcard/roms/` on the SD card
2. Launch HowBoyAdvance from the Tanmatsu app menu

## Build

```bash
# Prepare SDK (first time only)
make prepare

# Build
make build DEVICE=tanmatsu

# Upload (put Tanmatsu in badgelink mode first)
make install DEVICE=tanmatsu

# Or manually:
cd badgelink/tools && source .venv/bin/activate && \
python badgelink.py appfs upload howboyadvance "HowBoyAdvance" 0 ../../build/tanmatsu/application.bin

# Monitor serial output
make monitor DEVICE=tanmatsu PORT=/dev/ttyACM0
```

Windows users can also use `build.bat` and `upload.bat` helpers.

## Architecture

- **main/main.c** - Application entry point, ROM loading, emulation loop, input handling, display scaling
- **components/mgba/** - mGBA 0.10.4 core (ARM/Thumb CPU, GBA hardware emulation, software renderer)
  - `util/vfs/vfs-file.c` - Virtual filesystem with optimized staged SD card reads
  - `util/memory.c` - Memory allocation routing large buffers to PSRAM
  - `gb/audio.c` - GB audio (PSG shared by GBA audio subsystem)

## Known Issues

- Always delete `build/tanmatsu/esp-idf/main/libmain.a` before rebuilding to force recompilation
- 32MB ROMs do not fit in PSRAM (16MB and smaller work fine)
- FPS varies by scene complexity (~26-64 FPS with frame skipping)

## License

mGBA is licensed under the Mozilla Public License 2.0.
