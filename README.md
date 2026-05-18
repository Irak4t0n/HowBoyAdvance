# HowBoyAdvance

A Game Boy Advance emulator for the [Tanmatsu](https://tanmatsu.badge.team/) handheld console.

Sister project to [HowBoyMatsu](https://github.com/Irak4t0n/HowBoyMatsu) (Game Boy Color emulator).

## Hardware

- **SoC**: ESP32-P4 (400MHz dual-core RISC-V)
- **RAM**: 32MB PSRAM
- **Flash**: 16MB
- **Display**: 800x480 MIPI DSI
- **Input**: QWERTY keyboard + navigation keys

## Emulator

Uses [mGBA](https://mgba.io/) 0.10.4 core for GBA emulation.

- GBA screen (240x160) scaled 2x → 480x320, centered on 800x480
- Software renderer, 32-bit color converted to RGB888 for PAX display
- Save files stored on SD card

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
cd badgelink/tools && source .venv/bin/activate && \
sudo python badgelink.py appfs upload application "HowBoyAdvance" 0 ../../build/tanmatsu/application.bin

# Monitor serial output
cd ~/HowBoyAdvance && sudo chmod 666 /dev/ttyACM0 && make monitor DEVICE=tanmatsu PORT=/dev/ttyACM0
```

## Known Issues

- Always delete `build/tanmatsu/esp-idf/main/libmain.a` before rebuilding to force recompilation
- ROM files must be actual non-zero byte files on the SD card
- `ftell()` can return 0 on FAT filesystem — the loader uses a chunk-reading fallback

## License

mGBA is licensed under the Mozilla Public License 2.0.
