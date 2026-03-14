# SimCoupe ESP32-P4-PC

Port of the [SimCoupe](https://github.com/simonowen/simcoupe) SAM Coupé emulator to the [Olimex ESP32-P4-PC](https://www.olimex.com/Products/IoT/ESP32-P4/) board, running at full speed (50 fps / 100%) with audio and video.

## Hardware Required

| Component | Details |
|-----------|---------|
| **Board** | Olimex ESP32-P4-PC (Rev B or later) |
| **Display** | Any HDMI monitor (connected via the board's HDMI port) |
| **Keyboard** | USB keyboard connected to the board's USB-A port |
| **Storage** | microSD card (≥1 GB, FAT32) |

The board provides: ESP32-P4 dual-core RISC-V @ 400 MHz, 32 MB PSRAM, 16 MB flash, HDMI output via LT8912B bridge, audio via ES8311 codec, USB host via FE1.1s hub.

## Software Required

- [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/) (with ESP32-P4 support)
- Python 3.8+
- CMake 3.16+

Install ESP-IDF following the [official guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/get-started/index.html).

## Building

```bash
# Source the ESP-IDF environment
source ~/esp/esp-idf/export.sh

# Clone the repo (with submodules)
git clone --recurse-submodules <repo-url>
cd SamCoupe

# Incremental build
./build_olimex_p4pc.sh

# Clean build (required after changing sdkconfig.defaults or adding components)
./build_olimex_p4pc.sh clean

# Build and flash
./build_olimex_p4pc.sh flash

# Build, flash, and open serial monitor
./build_olimex_p4pc.sh monitor
```

Or use `idf.py` directly:

```bash
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

## SD Card Setup

Format the microSD card as FAT32 and create the following structure:

```
/sdcard/
└── simcoupe/
    ├── samcoupe.rom      ← SAM Coupé ROM (required)
    ├── SimCoupe.cfg      ← optional config file
    └── disks/            ← disk images (.dsk, .mgt, .sad, .sdf)
```

### ROM

The SAM Coupé ROM (`samcoupe.rom`, 32 KB) is copyright by Miles Gordon Technology and is **not included**. You can obtain it legally from:

- The [SimCoupe releases page](https://github.com/simonowen/simcoupe/releases) (included in the Windows installer)
- Dump it from real SAM Coupé hardware

Place `samcoupe.rom` at `/sdcard/simcoupe/samcoupe.rom`.

### Disk Images

Copy `.dsk`, `.mgt`, `.sad`, or `.sdf` disk images to `/sdcard/simcoupe/disks/`. Use the in-emulator menu (F12) to insert them into the virtual drives.

## Usage

### Special Keys

| Key | Action |
|-----|--------|
| **F1** | Insert disk into Drive 1 |
| **Shift+F1** | Eject disk from Drive 1 |
| **Alt+F1** | New blank disk in Drive 1 |
| **Ctrl+F1** | Save disk in Drive 1 |
| **F2** | Insert disk into Drive 2 |
| **Shift+F2** | Eject disk from Drive 2 |
| **Alt+F2** | New blank disk in Drive 2 |
| **Ctrl+F2** | Save disk in Drive 2 |
| **F3** | Tape browser |
| **Shift+F3** | Eject tape |
| **F4** | Import data |
| **Shift+F4** | Export data |
| **Alt+F4** | Exit emulator |
| **F5** | Toggle TV aspect ratio (5:4) |
| **F6** | Toggle graphics smoothing |
| **Shift+F6** | Toggle motion blur |
| **F8** | Toggle fullscreen |
| **F9** | Debugger |
| **Shift+F9** | Save screenshot (PNG) |
| **F10** | Options menu |
| **F11** | NMI (non-maskable interrupt) |
| **F12** | Reset SAM Coupé |
| **Ctrl+F12** | Exit emulator |

### Loading Software

1. Press **F1** to open the disk browser for Drive 1
2. Navigate to your disk image and press Enter
3. The SAM Coupé will auto-boot from the disk (or type `BOOT` in BASIC)

## Video Output

| Parameter | Value |
|-----------|-------|
| Resolution | 1280×720 (720p) |
| Pixel clock | 80 MHz (PLL_F240M/3, exact) |
| Refresh | ~60 Hz (htotal=1650, vtotal=808) |
| Bridge | LT8912B MIPI DSI → HDMI |
| SAM area | 512×192 scaled 2×H 3×V → 1024×576, centred |
| OSD area | 512×384 scaled 2×H 1.5×V → 1024×576, centred |

## Audio Output

| Parameter | Value |
|-----------|-------|
| Codec | ES8311 via I2S |
| Sample rate | 22050 Hz mono |
| SAA-1099 oversample | 2× |

## Performance

Measured on Olimex ESP32-P4-PC (ESP32-P4 @ 400 MHz):

| Metric | Value |
|--------|-------|
| Emulation speed | **100% (50 fps stable)** |
| Z80 execution | ~12.5 ms/frame (Core 1) |
| Sound synthesis | ~4 ms/frame (Core 0, overlapped) |
| Video blit | ~2 ms/frame (dirty-line tracking) |

## Credits

- **SimCoupe** original emulator by [Simon Owen](https://github.com/simonowen/simcoupe)
- **SAASound** SAA-1099 emulation by [Dave Hooper](https://github.com/stripwax/SAASound)
- **LT8912B driver** based on Linux kernel `drivers/gpu/drm/bridge/lontium-lt8912b.c`
- ESP32 port by Ramon Martinez (RQ-Radionics / Jorge Fuertes)

## License

SimCoupe is licensed under the GNU GPL v2. See [LICENSE](LICENSE) for details.
This ESP32 port follows the same license.
