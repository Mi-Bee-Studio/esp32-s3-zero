# env-station — Environment Station (0.96" OLED + DHT temperature & humidity + left/right buttons + CSI presence detection)

[中文文档](README.zh.md) | [English](README.md)

**Status (2026-09-26, firmware 0.1.0) — adds the network-probing capability (blackbox merged in); first board to implement the WFP protocol v3/v3.1**:
UDP :7789 join broadcast (discovered zero-config by homepulse — the PC-side IoT platform — which opens a session automatically) + hello identity handshake +
PC link liveness (30s without platform commands → local autonomy) + on-device local presence estimation (#PRES line) +
**enhanced protocol** (while the PC is on-link it samples and learns board-specific thresholds, pushed down via sense_cfg and persisted in NVS).
Since v2.5 the on-board web maintenance page (`http://<device IP>/`: Wi-Fi provisioning / OTA / reboot) and the
watchdog are standard. The temperature & humidity sensor is a **DHT22** (IO3); CSI follows the homepulse node route
(#S1/#ENV). The historical troubleshooting log is at the end.

## Target form

- 0.96" OLED (128×64) shows temperature & humidity and presence status; left/right buttons page/interact;
- Two routes for CSI presence detection (decided at the firmware stage; both can coexist):
  1. **homepulse node**: raw CSI data goes over USB serial (relayed by serialtap — the USB-serial middleware) up to the PC,
     where the DSP runs in homepulse — same structure as the luatos C3 sensing terminal; #S1 protocol experience carries over directly;
  2. **on-device coarse detection**: the S3's dual cores at 240 MHz have headroom; amplitude-variance-style thresholds
     decide presence, and the OLED shows the result directly.

## Hardware list

| Component | Model | Qty | Notes |
|------|------|------|------|
| Board | Waveshare ESP32-S3-Zero | 1 | board info: [../README.md](../README.md) |
| Display | 0.96" SSD1306 OLED, 128×64 | 1 | I2C 4-pin version |
| Buttons | 6×6mm tactile switches (left/right) | 2 | passive, non-polarized |
| Temperature & humidity | DHT11 or DHT22 | 1 | 1-Wire-style single bus, DATA on IO3 |

---

## Component specifications

### 0.96" OLED (SSD1306)

| Parameter | Value |
|------|-----|
| Resolution | 128 × 64 (monochrome) |
| Driver IC | SSD1306 |
| Interface | I2C (4 pins, most common) or SPI (7~8 pins, less common) |
| I2C address | 0x3C (default) / 0x3D (rare) |
| Supply | 3.3 ~ 5 V, **this project wires it to 3V3 throughout** |
| I2C speed | ≤400 kHz |
| Current | ~10–25 mA fully lit |
| Pins | GND / VCC / SCL / SDA —— **SDA/SCL silkscreen order varies by vendor; trust the silkscreen, not the position** |

> If your panel is the SPI version (DC / CS / RES / BLK silkscreen among the pins), the wiring is a different set —
> see "SPI-version OLED fallback" at the end of this document; don't wire it up following the I2C table first.

### Left/right buttons

- Left button = IO1, right button = IO2 (symmetric wiring; freely swappable in firmware);
- One leg to a GPIO, the other to GND, GPIO uses the internal pull-up, **pressing reads 0**;
- Debouncing is left to software (v0 firmware: 20 ms double sampling; the production version switches to interrupt + queue).

### DHT temperature & humidity sensor (DHT11 vs DHT22)

| Parameter | DHT11 | DHT22 / AM2302 |
|------|-------|----------------|
| Interface | 1-Wire-style single bus (1 data line) | 1-Wire-style single bus (1 data line) |
| Temperature range | 0 ~ 50 ℃ | −40 ~ 80 ℃ |
| Temperature accuracy | ±2 ℃ | ±0.5 ℃ |
| Humidity range | 20 ~ 90 %RH | 0 ~ 100 %RH |
| Humidity accuracy | ±5 %RH | ±2~5 %RH |
| Fastest sampling | 1×/second | 1×/2 seconds |
| Supply | 3.3 ~ 5.5 V | 3.3 ~ 6 V |
| Data format | 8-bit integer RH / T | 16-bit, temperature signed ×0.1 |

- **Identical wiring for both**: VCC→3V3, DATA→IO3, GND→GND;
- Bare part (4 pins, no carrier board): DATA needs a 4.7~10 kΩ pull-up to 3V3; three-pin blue carrier-board modules
  have it on-board — wire up directly;
- **Telling the models apart**: by looks (small blue square shell = DHT11; white large-mesh shell one size bigger = DHT22)
  or by silkscreen (AM2302 = DHT22). You don't have to guess: the v1 firmware decodes as DHT22 first and falls back
  to DHT11 decoding if the values look unreasonable — automatic discrimination;
- Alternative (not currently used): SHT30/SHT40 are I2C temperature & humidity sensors (0x44, ±0.2 ℃ / ±2 %RH,
  10 Hz) — if the DHT ever feels too slow/crude, just attach one to the same I2C bus at IO8/9; the driver is even
  simpler than the DHT's.

---

## Connecting to the board

### Design principles

The OLED owns the I2C bus (IO8/9) exclusively, the DHT's single bus owns IO3 exclusively, each button takes one pin —
no conflicts; CSI rides the WiFi radio and takes no GPIO at all.

### Pin assignment (revised 2026-09-22 after wide-probe (I2C scan) measurements, to match the actual wiring)

| Function | GPIO | Status | Notes |
|------|------|------|------|
| I2C SDA | **IO6** | ✅ measured (OLED 0x3C acks) | OLED SDA |
| I2C SCL | **IO5** | ✅ measured | OLED SCL |
| Left button | **IO1** | pending button test | other leg to GND, internal pull-up |
| Right button | **IO2** | pending button test | same |
| DHT DATA | **IO3** | not wired / unverified | strapping caveat in the next section |
| (on-board) BOOT button | IO0 | on-board | can stand in as a "third button" |
| (on-board) WS2812 | IO21 | on-board | not claimed by this project for now |

> Measurement note: the wiring differs from the original plan (I2C=IO8/9); the wide-probe firmware found the OLED
> actually on SDA=IO6, SCL=IO5 (reversed). Later firmware follows the actual pins; IO8/9 are freed.

### ⚠ IO3 is a strapping pin (read before attaching the DHT)

- IO3 is sampled at reset and selects the **JTAG source**: 1 = USB-Serial-JTAG (default, weak
  pull-up on the pin), 0 = external JTAG pins (IO39~42). This **only affects JTAG debug routing — it does not
  affect USB serial logs, nor serialtap/esptool flashing**;
- The DHT's DATA line rests high via the pull-up → the power-on sample reads 1, exactly the default state; no impact;
- The only risk window: the DHT happens to be mid-communication when RESET is pressed and pulls the line low →
  that one boot routes JTAG to the external pins. The consequence is minor (a power cycle restores it); if you'd
  rather not worry, just move to IO4/IO5/IO6 — one macro changes in wiring and firmware;
- Firmware note: RMT reads the DHT as "drive low → release → read"; never configure IO3 as a constant output-low.

### Wiring tables

**OLED (I2C version)**:

| OLED pin | Connect to board |
|---------|--------|
| GND | GND |
| VCC | 3V3 |
| SCL | IO9 |
| SDA | IO8 |

**DHT11 / DHT22**:

| Sensor pin | Connect to board |
|----------|--------|
| VCC (+) | 3V3 |
| DATA (out) | IO3 |
| GND (−) | GND |

Bare part extra: NC left floating, 4.7~10 kΩ pull-up resistor between DATA and 3V3.

**Buttons ×2**: one leg of the left button to IO1, one leg of the right button to IO2, other legs both to GND.

```
3V3 ●──┬── OLED VCC
       └── DHT VCC
GND ●──┬── OLED GND
       ├── DHT GND
       └── other legs of both buttons
IO8 ●── OLED SDA       IO9 ●── OLED SCL
IO1 ●── left button    IO2 ●── right button
IO3 ●── DHT DATA
```

### Electrical notes

- **Everything runs on 3V3**: DHT11/22 work fine at 3.3 V; don't power them from 5 V — then DATA
  highs are 5 V and would damage the ESP32's IO;
- The board exposes only one GND header hole; for multiple devices, converge onto this one hole via a
  breadboard ground rail;
- I2C pull-ups: the OLED module has on-board pull-ups, no need to add more (the v0 firmware also enabled
  internal pull-ups as a fallback);
- The DHT is timing-sensitive: the production firmware uses the **RMT peripheral** for capture, not bit-bang;
  sampling interval ≥1s (DHT11) / ≥2s (DHT22);
- Power budget: OLED fully lit ~25 mA + DHT ~1 mA + S3 fully loaded (WiFi bursts included) a few hundred mA —
  USB power is entirely worry-free.

### CSI presence detection (new capability)

- **Capability**: the ESP32-S3 supports WiFi CSI (`esp_wifi_set_csi`);
  `sdkconfig.defaults` already enables `CONFIG_ESP_WIFI_CSI_ENABLED` as a reservation;
- **Stimulus**: CSI only fires when there is WiFi traffic, so it needs continuous stimulus — the C3 project found
  pinging the gateway most reliable in testing; put this board and the stimulus source on the same router;
- **Link positioning**: on the homepulse node route, CSI frames go over USB serial, relayed by serialtap
  up to the PC (the panel at :8801 can watch the byte stream directly); DSP/multipath subtraction live in
  homepulse. The on-device coarse-detection route closes the loop on this board and only uploads results. The
  firmware stage picks one or both;
- **Placement**: CSI is extremely sensitive to multipath — once position and orientation are fixed, don't move it
  around often; keep the OLED and its cable off the end with the on-board PCB antenna, and keep the whole device
  away from metal.

### SPI-version OLED fallback (only if the panel is the 7~8-pin version)

The OLED goes on SPI2, the DHT stays on IO3 as before: SCK=IO12, MOSI=IO11, CS=IO10, DC=IO13,
RES=IO5, BLK floating or IO6. Wire it only after confirming it's the SPI version; configure the firmware for the SPI panel.

---

## Buttons & UI

Two-color split: **yellow band = status bar** (page name / Wi-Fi signal bars /
streaming heartbeat dot / PC-link mark), **blue area = data**, bottom row = key
hints (yields to a toast receipt for 1.5 s after each action).

Three pages (L short press cycles):

| Page | Content |
|------|---------|
| **ENV** | Big temperature / humidity (°C / %), dew point (Magnus), read counter |
| **CSI** | Presence verdict (IN/OUT; platform verdict when PC linked, else local), motion bar with learned threshold ticks (sense_cfg), sample rate |
| **SYS** | Uptime / heap / CSI total / IP / DHT reads & errors / probe success count |

Button grammar (same set as the luatos sensing terminal):

| Key | Action |
|-----|--------|
| L short | Next page ENV → CSI → SYS |
| L long 1 s | Streaming toggle (global, with toast) |
| R short | Page action: ENV = force read · CSI = streaming toggle · SYS = diagnostic snapshot to log |
| R long 1 s | Reboot into ROM download mode (rescue flashing, unchanged) |
| L+R together 2.5 s | Soft reboot (screen countdown at 1.5 s) |
| 10 min idle | Auto screen off (anti burn-in; GRAM kept, sensing unaffected); then **random peeks**: every 1-5min the screen lights for 1-60s showing either the live page or an expression animation (blink / happy face); any key wakes (that key only wakes, fires nothing) |

## v0 probe firmware


Behavior:

1. On boot prints heap / PSRAM size (verifies the Octal PSRAM configuration);
2. Full-range I2C scan (0x08~0x77): a hit on 0x3C/0x3D is labeled OLED, 0x44/0x45 labeled
   I2C temperature & humidity (SHT alternative); if nothing shows up, it prompts to check the wiring;
3. Prints on any state change of the left button (IO1) / right button (IO2) (pressed=0 / released=1,
   20 ms double-sample debounce);
4. One heartbeat every 10 seconds.

Not included in this version: DHT decoding (v1, RMT) and CSI (sensing stage).

Build (cached toolchain driving ninja directly; verified on this machine; artifact ~192KB):

```bash
cd env-station
export IDF_PATH=~/esp/.espressif/v6.0/esp-idf   # 必须 export，cmake 内部取 $ENV{IDF_PATH}
export PATH="/c/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:\
/c/Espressif/tools/python/v6.0/venv/Scripts:\
/c/Espressif/tools/ninja/1.12.1:/c/Espressif/tools/cmake/4.0.3/bin:$PATH"
cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=$IDF_PATH/tools/cmake/toolchain-esp32s3.cmake
ninja -C build
```

Two Git Bash-specific pitfalls (solved within this project; recorded for reference):

- **MSys detection**: the `idf_tools.py` entry point rejects MSys, and the MSYS runtime re-injects `MSYSTEM`
  into native child processes, so `unset` in the shell is ineffective — the top-level CMakeLists already uses
  `idf_build_set_property(__CHECK_PYTHON 0)` to skip that pre-check (the dependencies themselves are installed in
  `/c/Espressif/tools/python/v6.0/venv`; the check was a false positive);
- **Occasional compiler ICE**: IDF's bundled `esp_lcd` RGB source files very rarely hit an internal segfault
  under gcc 15.2.0; re-running `ninja -C build` once gets past it — unrelated to this project's code.

**After changing `sdkconfig.defaults` you must `rm -f sdkconfig` and re-run cmake** — the root
`sdkconfig` takes priority over the defaults; if you don't delete it, changes are silently shadowed (measured
in this project: changed `SPIRAM=n` without deleting sdkconfig, and the rebuilt firmware was still Octal — it
hung the moment it was flashed). Variant builds (`build-nopsram/`, `build-psram40/`, `build-quad/`) point
`-DSDKCONFIG` at their own build directories and are unaffected by the root sdkconfig.

Flashing (when serialtap owns the board, flash through the proxy; since v2.3 the OTA dual-slot partition
table is in place, **app offset 0x20000**; the first table switch needs partition table + app flashed, bootloader unchanged):

```bash
serialtap flash <device regex> \
  build/partition_table/partition-table.bin@0x8000 \
  build/env-station.bin@0x20000
```

---

## Next steps

1. Wire OLED + left/right buttons → flash v0: confirm the 0x3C and left/right button logs;
2. DHT on IO3 → v1: RMT decoding (auto DHT11/22 discrimination), temperature & humidity in the log;
3. Light up the OLED: esp_lcd + SSD1306 monochrome hand-drawn UI (no LVGL at 128×64 — the luatos
   project's LVGL lesson is a net liability on this small panel), temperature & humidity on the first screen;
4. CSI stage: WiFi association + `esp_wifi_set_csi` callback capture → first verify the stimulus via logs,
   then decide homepulse node (reusing #S1 experience) or on-device coarse detection;
5. Throughout, observe logs and byte streams via the serialtap panel.

---

## Current troubleshooting log (2026-09-22)

**PSRAM conclusion (settled)**: the nominal Octal 2MB does not match the actual part — `SPIRAM_MODE_OCT`
at both 80M **and 40M** gives `octal_psram: PSRAM chip is not connected, or wrong
PSRAM line mode` → abort → 77ms boot-abort loop → bus-level hang (soft reset useless,
the USB device vanishes from the bus; only cutting power helps). esptool reports chip v0.2 (2021-03 ROM),
"Embedded PSRAM 2MB (AP_3v3)". **The project default is now `CONFIG_SPIRAM=n`**
(sdkconfig.defaults); the Quad variant (`build-quad/`) is untested. Recovery procedure for a board hung in
PSRAM init: **a normal USB unplug/replug (don't press BOOT) + the `serialtap/catch-flash.sh`
port-watch script** — after detecting the port's "disappear → reappear" edge, it lets esptool hold the chip
in ROM download mode (`--after no-reset`), then flashes the no-PSRAM firmware back through the proxy, fully automatic.

**v1 runtime verification (nopsram build)**: the board boots normally, `heap≈397KB`, heartbeat 10s/entry
stable, left/right buttons (IO1/IO2) verified working (IO1 27 / IO2 43 events). **OLED located
(wide-probe)**: SDA=**IO6**, SCL=**IO5** (0x3C acks; the v1 SSD1306 driver
lights it up fine). **DHT: after fixing the RMT RX glitch-filter parameter overrun (`signal_range_min_ns`
must be <3187ns; a mistaken 15000ns meant RX never armed and everything timed out), v1 with auto-scan confirmed
no response on any of IO3/4/7~13 — a sensor-itself/wiring problem, under investigation (VCC=3V3,
GND, DATA→IO3; the bare part needs a 4.7~10k pull-up, modules have one on-board).**

**v1 full-chain wrap-up (2026-09-22 21:00)**: OLED (IO6/IO5) lit, showing temperature & humidity; left button pages
(temperature & humidity / diagnostics), right button reads immediately, **right button long-press 1s reboots into ROM
download mode** (flash-it-yourself rescue without unplugging); **DHT22@IO3 periodic capture works**
(2.5s each, model auto-identified, log carries raw bytes), heartbeat carries temperature & humidity.

**v2.2 current form (2026-09-22 22:55)**:
- **Two-tone display UI**: yellow band (rows 0-15) = page title, blue area (rows 16-63) = data; three pages cycling
  (left button): temperature & humidity large digits / diagnostics / CSI status (with PC:IN/OUT presence display);
- **Temperature & humidity calibration**: `cal <toff> <rhoff>` (persisted in NVS; query with `cal?` — the DHT22 self-heats when mounted
  against the board; the physically right fix is an extension lead to move the sensor away);
- **CSI/homepulse interoperability**: `#S1-HELLO`+`#S1` in the same format as luatos; responds to
  sense_start (stimulus_hz adjusts the ping rate) / sense_stop / sense_status (present shown on the panel).
  Credentials pushed via the `wifi <ssid> <pass>` serial command (NVS wifi/ssid,pass);
- **Command line**: driver-API reads (fgets doesn't work; copied luatos's
  usb_serial_jtag_read_bytes pattern); right button long-press 1s enters download mode; main-loop watchdog 5s.
- **Flashing**: in v2.x, once the resident serial reader is running, long-run-state flashes hit first try
  (direct flash via serialtap flash).

**v2.5 on-board web maintenance page + OTA + watchdog (2026-09-24)**:
- **Partition table switched to dual-slot OTA** (`partitions.csv`: ota_0/ota_1 at 1984K each, nvs/phy
  positions unchanged — calibration and credentials migrate losslessly; app offset 0x10000 → **0x20000**);
- **app_web.c (frontend and backend in one file, :80)**: embedded single page (status card / Wi-Fi provisioning
  form / firmware upload XHR progress bar / reboot) + esp_http_server backend (GET /, /api/status, POST
  /api/wifi, /api/reboot, **POST /ota** — stream-write to the spare slot → esp_ota_end verify
  (on failure no slot switch, original firmware unharmed) → switch slot → delayed 1s reboot);
- **Always-on SoftAP "env-station"/12345678** (APSTA): when unprovisioned or offline,
  192.168.4.1 is always the rescue channel — provisioning doesn't depend on the serial port; connect a phone
  to the hotspot to push WiFi credentials and OTA-flash; after STA association the AP channel follows
  automatically, CSI unaffected;
- **Watchdog hardening**: main loop (20ms) + DHT task (2.5s) both subscribe to the watchdog,
  `CONFIG_ESP_TASK_WDT_PANIC=y` makes timeouts actually trigger reboot (previously the in-code
  trigger_panic returned INVALID_STATE because the system TWDT already existed and was swallowed; panic must go
  through sdkconfig to take effect);
- Same-structure copies: blink (self-contained WiFi version) and blackbox (dual-server version) landed the same
  day; luatos, constrained by 2MB flash, only added the watchdog (see [blink/AGENTS.md](../blink/AGENTS.md) —
  maintenance-page/OTA/watchdog engineering conventions).

**v2.6 WFP protocol v3: zero-config discovery + two-way handshake + local autonomy (2026-09-26)**:
- **Join broadcast (app_disc.c)**: once it has an IP, broadcasts a `#HELLO {devid,proto,fw,name,caps,ip,tcp}`
  line to 255.255.255.255:7789 every 10s; on receiving a PC broadcast
  `{"cmd":"wfp_probe"}` it replies immediately by unicast. homepulse, listening on :7789, thereby identifies boards
  **never configured** and opens TCP sessions automatically (static config takes priority; dedup by same IP/name);
- **hello handshake**: `{"cmd":"hello"}` → `#HELLO {...}` (devid derived from the efuse MAC);
- **PC link liveness**: any platform command (hello/sense_*/wifi_*) renews the link; **30s without a command
  falls back to local autonomy** — the OLED presence display switches from `PC:IN/OUT` (platform authority) to
  `LOC:IN/OUT` (local estimate), and it uploads `#PRES {"seq","present","src":"csi-local","m"}`;
- **Local presence estimation**: EWMA of the frame-to-frame difference of CSI amplitude (|I|+|Q|), double-threshold
  hysteresis (enter>25 exit<12) + debounce counting; during the 600-frame warm-up with no action the default is
  "nobody present". Threshold observation/tuning: serial `pres?`, heartbeat `m=`, /api/status `motion`;
- **Windows firewall note**: without an inbound rule on the PC side, the broadcast announcements are blocked —
  homepulse's subnet unicast scan fallback still discovers within ≤30s (replies ride the established-connection
  pinhole); after adding a UDP 7789 inbound rule, the broadcast path gets through immediately.

**v2.7 enhanced protocol (2026-09-26 evening, WFP v3.1)**: while the PC is on-link, homepulse samples the board's
motion metric m every 5s via `sense_stat` and, against its DSP verdicts, learns **this board's own dual thresholds**,
pushed down via `sense_cfg` (NVS "env"/moten,motex ×0.1, persisted; still in effect when offline) —
"stronger with the PC attached, no regression when the PC is unplugged". Threshold observation: serial `pres?`,
`/api/status` mot_enter/mot_exit. **Dual-channel coexistence semantics** (protocol §6): telemetry is dual-homed
output (one copy each on serial+TCP), commands are accepted on both channels, liveness is renewed by either channel.
Fundamental limit: a pure motion metric has no separability for a motionless sitting person (the PC's breathing-class
DSP backstops it).

**v3.0 network probing (2026-09-26, esp32-blackbox merged in)**: this board has ample headroom (47% free in its
OTA slot), so the blackbox capability joins the same firmware as a fourth capability:

- **Probing core byte-identical to upstream** (probe_http/tcp/dns/icmp/ws.c — 8 prober types: HTTP/HTTPS/TCP/
  TCP+TLS/DNS/ICMP/WS/WSS), wired through the `app_probe.c` adapter layer;
- **Config backend SPIFFS → NVS** (this board's 4MB flash is fully consumed by the OTA dual slots, so there is no
  storage partition; namespace `probe` / key `cfg`, JSON ≤3.5KB, schema fully upstream-compatible);
- **Metrics endpoints merged into the maintenance page on :80** (no separate :9090 server): `GET /metrics`
  (Prometheus scrape), `GET /probe?target=X&module=Y[&port=P]` (exporter-compatible on-demand probe),
  `GET/POST /api/probe` (config read / submit with hot reload); the maintenance page gains a probe card
  (target status table + JSON editor);
- Console: `probe?` / `probe run <name>` / `probe reset`;
- The probe task does not join the TWDT (the 5s main-loop watchdog keeps its sensitivity; probes are bounded by
  the 1-120s module timeout) and idles while WiFi is disassociated. Factory defaults ship 4 standard modules and
  0 targets (a home device makes no unsolicited outbound calls — use /probe on demand or add targets in the
  dashboard).

**serialtap port-leak fix (2026-09-22, a side project feeding back into the middleware)**: the root cause of a port
staying busy forever after a proxy session + pause was an inverted defer order in collectOnce (Close ran before
the write-sink removal; under Windows overlapped IO, Close fails silently and the handle leaks). Fixed in
internal/collector/collector.go and regression-verified; remember `-stop` after using the proxy.

**Four RMT DHT driver pitfalls (all fixed; note for reuse)**: ① `mem_block_symbols` ≥
SOC_RMT_MEM_WORDS_PER_CHANNEL (S3=48) and even; ② `signal_range_min_ns`<3187;
③ `signal_range_max_ns`<32767000; ④ **don't use RMT TX for the start pulse** (push-pull + idle low
holds the data line at GND constantly and the sensor stays mute forever) — drive the GPIO open-drain low for
20ms then switch to input. When parsing, release artifacts add one extra ~30us high pulse: with nh=42 drop the
first 2, with nh=41 drop the first 1.

**Early incident** (same day): the factory firmware, compiled for 8MB flash, rebooted endlessly on this 4MB board —
the root cause of the flaky port ("works one minute, not the next", ERROR 31 etc.) before flashing; not a wiring problem.

## Hardware occupancy

| Peripheral | GPIO | Status |
|------|------|------|
| I2C (OLED) | IO8 / IO9 | planned |
| Left / right buttons | IO1 / IO2 | planned |
| DHT DATA | IO3 | planned (strapping caveat) |
| SPI spare (SPI-version OLED) | IO5, IO10~13 | SPI-panel option only |
| WS2812 / BOOT | IO21 / IO0 | on-board, not claimed for now |
