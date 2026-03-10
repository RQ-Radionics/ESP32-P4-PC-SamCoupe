# Hardware: Olimex ESP32-P4-PC

## Board Overview

The [Olimex ESP32-P4-PC](https://www.olimex.com/Products/IoT/ESP32-P4/) is a single-board computer based on the ESP32-P4 SoC.

| Feature | Spec |
|---------|------|
| SoC | ESP32-P4, dual-core RISC-V HP @ 400 MHz + LP core |
| RAM | 768 KB internal SRAM + 32 MB PSRAM (octal SPI) |
| Flash | 16 MB SPI flash |
| Display | MIPI DSI → LT8912B → HDMI (up to 1080p) |
| Audio | I2S → ES8311 codec → 3.5mm jack + speaker header |
| USB | FE1.1s 4-port USB 2.0 hub (USB-A host ports) |
| Storage | microSD (SDMMC, 4-bit, up to UHS-I) |
| Connectivity | Ethernet |

## Peripherals Used by This Port

### HDMI Output (LT8912B)

The LT8912B is a MIPI DSI to HDMI bridge. The ESP32-P4 drives it via the DPI (parallel RGB) interface in this port (not MIPI DSI directly).

| Parameter | Value |
|-----------|-------|
| Resolution | 512×480 (active), 840×525 (total with blanking) |
| Pixel clock | 30 MHz |
| Refresh rate | ~71 Hz |
| Pixel format | RGB888 (24-bit) |
| Double buffering | Yes (2 × 737 KB in PSRAM) |

The LT8912B is configured via I2C (shared bus with ES8311). Driver: `components/esp_lcd_lt8912b/`.

Configuration in `sdkconfig.defaults`:
```
CONFIG_SIM_DISPLAY_HACT=512    # horizontal active pixels
CONFIG_SIM_DISPLAY_VACT=480    # vertical active pixels
CONFIG_SIM_DISPLAY_HBP=248     # horizontal back porch (includes 128px extra for 512-wide)
```

### Audio (ES8311)

The ES8311 is a low-power mono audio codec with I2S interface.

| Parameter | Value |
|-----------|-------|
| Sample rate | 22050 Hz |
| Bit depth | 16-bit stereo |
| MCLK | 5,644,800 Hz |
| I2S mode | Standard (Philips) |
| DMA frames | 441 samples (= 1 frame at 22050 Hz / 50 fps) |
| Volume | 80% (configurable) |

The ES8311 shares the I2C bus with the LT8912B. `sim_display_init()` must be called before `sim_audio_init_with_bus()`. Driver: `components/sim_audio/`.

### USB Keyboard (FE1.1s hub)

The FE1.1s is a USB 2.0 hub providing 4 USB-A ports. The ESP32-P4 acts as USB host.

- USB HID boot-protocol keyboard reports are decoded to PS/2 Set 2 scancodes
- Scancodes are delivered via a FreeRTOS queue to `Input::Update()`
- Driver: `components/sim_kbd/`

Supported: standard 104-key keyboards. USB hubs downstream of the FE1.1s are not supported.

### microSD Card (SDMMC)

| Parameter | Value |
|-----------|-------|
| Interface | SDMMC 4-bit |
| Filesystem | FAT32 |
| Mount point | `/sdcard` |
| Max speed | UHS-I (if card supports it) |

Driver: `components/sim_sdcard/`. The SD card is optional — the emulator boots without it (ROM only, no disk images).

## I2C Bus Sharing

Both the LT8912B (display) and ES8311 (audio) are on the same I2C bus. The bus is created by `sim_display_init()` and the handle is retrieved with `sim_display_get_i2c_bus()` for use by `sim_audio_init_with_bus()`.

Do not call `sim_audio_init()` (standalone) — it creates a conflicting I2C bus.

## PSRAM Architecture

The ESP32-P4's PSRAM is accessed via the MSPI (Multi-SPI) bus, shared between:
- CPU instruction/data cache misses (Z80 code, SAM RAM)
- DMA for display framebuffer (LT8912B reads)
- Explicit `esp_cache_msync()` calls (cache writeback for display)

**PSRAM bus contention is the main performance bottleneck.** Avoid concurrent PSRAM access from multiple cores where possible. See `docs/ARCHITECTURE.md` for details.

## Pin Assignments

The following pins are used by the drivers (defined in board support files):

| Function | Signal | Notes |
|----------|--------|-------|
| Display | MIPI DPI | Configured by `sim_display` component |
| Display I2C | SDA/SCL | Shared with audio |
| Audio I2S | BCLK/WS/DOUT | ES8311 |
| Audio I2C | (shared) | ES8311 config |
| USB host | D+/D− | Via FE1.1s hub |
| SD card | CLK/CMD/D0-D3 | SDMMC 4-bit |

Refer to the [Olimex ESP32-P4-PC schematic](https://github.com/OLIMEX/ESP32-P4-PC) for exact GPIO numbers.

## Errata / Known Issues

- **Rev A boards**: MIPI DSI signal integrity issues may cause display glitches. Use Rev B or later.
- **USB SET_PROTOCOL**: The `W (sim_kbd): SET_PROTOCOL failed (non-fatal)` warning is expected on some keyboards that don't support the boot protocol SET_PROTOCOL command. Input still works correctly.
- **simcoupe_task stack must be in DRAM**: `esp_cache_msync(DIR_C2M)` can transiently invalidate cache lines covering the active stack if it is in PSRAM, causing load access faults. The 32 KB DRAM stack is sufficient (SimCoupe's large buffers are heap-allocated in PSRAM).
