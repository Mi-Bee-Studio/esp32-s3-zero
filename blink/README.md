# blink — esp32-s3-zero baseline project

[中文文档](README.zh.md) | [English](README.md)

The first project in this repo to follow the board-centric repo convention:
it validates the minimal closed loop for this board (LED / button /
heartbeat logging / USB serial), and also gives serialtap (USB-serial
middleware) an S3 USB-Serial-JTAG device to validate the middleware layer.

## Behavior

- The WS2812 (GPIO21) steps through one color per second: green → amber →
  blue → dim (status color semantics match the homepulse/serialtap tray
  icons);
- Pressing the BOOT button (GPIO0) manually switches to the next color;
- A `heartbeat` log line (uptime / heap) every 10 seconds; on boot it prints
  the heap and PSRAM size (verifying that the Octal PSRAM configuration took
  effect);
- **On-board web maintenance page (:80, `main/app_web.c`)**: ships with its
  own WiFi APSTA and NVS-backed credentials — when not provisioned, the
  rescue SoftAP `blink-s3`/`12345678` at 192.168.4.1 offers Wi-Fi
  provisioning, firmware OTA upload (dual-slot; a failed verification does
  not switch slots), or a reboot; once joined to a network, access it via
  the STA IP;
- **Watchdog**: the main loop feeds it on a 1 s tick; a stall longer than
  5 s panics and reboots for self-recovery
  (`CONFIG_ESP_TASK_WDT_PANIC=y`).

## Build and flashing

```bash
cd blink
idf.py set-target esp32s3   # first time
idf.py build
idf.py -p COMx flash monitor

# Or flash via the serialtap proxy (recommended when the daemon owns the board)
serialtap flash <device regex> build/blink.bin@0x10000
```

Direct build in local Git Bash (the export script rejects MSys; use the
cached toolchain):

```bash
IDF_PATH=~/esp/.espressif/v6.0/esp-idf
ROMS=$(ls -d /c/Espressif/tools/esp-rom-elfs/*/ | head -1)
PATH="/c/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:\
/c/Espressif/tools/python/v6.0/venv/Scripts:\
/c/Espressif/tools/ninja/1.12.1:/c/Espressif/tools/cmake/4.0.3/bin:$PATH" \
  cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=$IDF_PATH/tools/cmake/toolchain-esp32s3.cmake
ninja -C build
```

## Hardware usage

| Peripheral | GPIO |
|------|------|
| WS2812 | 21 |
| BOOT button | 0 |

Everything else is left free (WiFi/OTA use the radio and flash, not GPIOs;
the partition table has been switched to dual-slot OTA with the app offset
at 0x20000, see `partitions.csv`). For the full hardware picture of the
board see [../README.md](../README.md).
