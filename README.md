# ESP32-S3-Zero (Waveshare)

[中文文档](README.zh.md) | [English](README.md)

[![Build Firmware](https://github.com/Mi-Bee-Studio/esp32-s3-zero/actions/workflows/build.yml/badge.svg)](https://github.com/Mi-Bee-Studio/esp32-s3-zero/actions/workflows/build.yml)

The first board under the board-centric repo convention. **This directory is organized by the "board as root" rule**:

```
esp32-s3-zero/
├── README.md          # this file: all hardware info for this board
└── <project>/         # one directory per project built on this board
    ├── CMakeLists.txt / main/ / sdkconfig.defaults / main/idf_component.yml
    └── README.md      # project description + build/flashing commands
```

Key points of the convention:

- **Board directory name** = board name (kebab-case); the root README covers hardware only, never project content;
- **Each project directory builds standalone**: it ships the full ESP-IDF project trio (top-level CMakeLists,
  `main/`, `sdkconfig.defaults`); `cd <project> && idf.py build` produces the firmware;
- Projects share no code; when commonality is needed, copy first, and consider extracting a shared component only once things stabilize.

---

## Board Overview

| Item | Value |
|------|-----|
| Module/chip | ESP32-S3FH4R2 — Xtensa LX7 dual-core 240MHz |
| Flash | 4MB (embedded in-chip) |
| PSRAM | 2MB Octal (embedded in-chip; **GPIO33–37 are used by it and not broken out**) |
| Wireless | 2.4GHz WiFi b/g/n + Bluetooth 5 (LE) |
| USB | **Native USB Type-C (USB-Serial-JTAG), no USB-UART bridge chip** |
| UART0 | TX=GPIO43, RX=GPIO44 |
| Onboard LED | **WS2812 RGB on GPIO21** (addressable, no concept of an active level) |
| Buttons | BOOT=GPIO0 (hold to enter download mode), RESET |
| Power | 3.3V LDO (ME6217C33M5G, 800mA); 5V pad input 3.7–6V, ≥500mA recommended |
| Breakout | Front: two rows of castellated half-holes 2×9 (18 holes) + back: one staggered row of half-holes (8 holes) + 3 pads (IO14/15/16); 24 user GPIOs in total |
| Dimensions | 18.00 × 23.50 mm (USB-C sticks out slightly) |

## Pinout Diagram (USB-C pointing up, front/component-side view)

```
                 ┌─ USB-C ─┐
        5V ◎┬───┘  [WS2812]├───┬◎ TX      ← TX = IO43
       GND ◎│      (IO21)  │   ◎ RX      ← RX = IO44
       3V3 ◎│ [BOOT] [RST] │   ◎ 13
       IO1 ◎│    ┌────┐    │   ◎ 12
       IO2 ◎│    │S3  │    │   ◎ 11        14/15/16: solder
       IO3 ◎│    │FH4R2    │   ◎ 10        pads (not header
       IO4 ◎│    └────┘    │   ◎ 9         holes — solder or
       IO5 ◎│ [C3 power    │   ◎ 8         probe with pogo
       IO6 ◎│  area]       │   ◎ 7         pins)
            └──────────────┘
             left row (front)  right row (front)

  Back (silkscreen side): a staggered row of half-holes sits in the gaps of
  the long edge, from the USB end:
  IO45 · IO42 · IO41 · IO40 · IO39 · IO38 · IO18 · IO17
  (IO45 is a strapping pin; check the back silkscreen before using)
```

Key points:

- Front **left row** (downward from the USB end): `5V, GND, 3V3, IO1, IO2, IO3, IO4, IO5, IO6`;
- Front **right row** (downward from the USB end): `TX, RX, IO13, IO12, IO11, IO10, IO9, IO8, IO7`;
- **Back staggered row**: `IO45, IO42, IO41, IO40, IO39, IO38, IO18, IO17` (they sit in the gaps of the
  front header rows; when soldering headers, remember to solder from both front and back);
- IO14/15/16 have only round pads; IO0 = BOOT button; IO21 = WS2812; IO19/20 = USB;
- IO33–37 are not broken out (taken by Octal PSRAM).

## Caveats

- **No USB-UART bridge**: the serial port is the S3's own USB-Serial-JTAG. DTR/RTS behavior after
  open matches the C3 (serialtap, our USB-serial middleware, already handles this: it releases
  DTR/RTS immediately after open to prevent reset pulses).
- **Entering download mode**: the board has no auto-download circuit — hold BOOT while plugging in
  USB or pressing RESET. But when flashing through serialtap's proxy, esptool automatically tries a
  reset into ROM download mode, so manual presses are usually unnecessary.
- GPIO19/20 are USB D-/D+ — don't repurpose them; GPIO0 is the BOOT button, and its power-on state
  decides the boot mode — it can be used as an input, but mind its power-on level.
- Avoid GPIO35–37 (PSRAM) and GPIO43/44 (UART0).
- **⚠ PSRAM tested on real hardware (2026-09-22)**: rated Octal 2MB, but this unit (chip v0.2,
  2021-03 ROM) dies right at `SPIRAM_MODE_OCT` init at 80M **and 40M** alike with
  `octal_psram: PSRAM chip is not connected, or wrong PSRAM line mode` →
  abort → a 77ms boot-abort loop, then a bus-level hang (soft reset is useless; only a power cycle helps).
  esptool reports "Embedded PSRAM 2MB (AP_3v3)" — i.e. the silicon believes it has embedded PSRAM,
  but the Octal wiring never initializes — **until this is understood, this project always sets `CONFIG_SPIRAM=n`**.
  Quad mode still untested. Lesson: this kind of hang makes USB vanish entirely; for the recovery flow see
  env-station's catch-flash scheme (BOOT-free: power-cycle re-plug + port-watch parking it into download mode).

## Serialtap (middleware) integration points

- On USB it enumerates as USB-Serial-JTAG, with a device-name shape identical to the C3 (e.g. `esp32s3-jtag`);
  serialtap's existing discovery/naming/capture/passthrough/flashing pipeline should just work —
  pending verification once the real board is hooked up;
- Writing 4MB of flash takes about the same order of time as the C3 sensing terminal (~1.4MB);
  the flashing milestone event stream can be reused directly;
- The WS2812 (GPIO21) can serve as a "device status LED"; later on, serialtap/homepulse could push
  color commands through the proxy (implemented in the board-side project).

## Project Index

| Project | Description |
|------|------|
| [blink](blink/README.md) | Baseline project: WS2812 heartbeat color pulse + BOOT-button color switching + heartbeat logging (the first project landed under this convention) |
| [env-station](env-station/README.md) | Environment mini-station (v3.0, with blackbox network probing): 0.96" SSD1306 + left/right buttons + DHT22 (IO3) + WiFi CSI presence detection; onboard web maintenance page (Wi-Fi provisioning/OTA) + watchdog; first board to implement the WFP protocol v3/v3.1 (zero-config discovery/handshake/local autonomy/threshold learning), see its README |
