# blink — esp32-s3-zero 基线工程

[English](README.md) | [中文文档](README.zh.md)

本仓库第一个按“主板目录规范”落地的项目：验证这块板的最小闭环
（LED / 按键 / 日志心跳 / USB 串口），也给 serialtap 提供一款 S3
USB-Serial-JTAG 设备做中间层验证。

## 行为

- WS2812（GPIO21）每秒步进一种颜色：绿 → 琥珀 → 蓝 → 暗（状态语义与
  homepulse/serialtap 托盘一致）；
- BOOT 键（GPIO0）按一下手动换一种颜色；
- 每 10 秒一条 `heartbeat` 日志（uptime / heap），启动时打印 heap 与
  PSRAM 容量（验证 Octal PSRAM 配置生效）；
- **板端 Web 维护页（:80，`main/app_web.c`）**：自带 WiFi APSTA 与凭据
  NVS——未配网时热点 `blink-s3`/`12345678` → 192.168.4.1 可配网、上传
  固件 OTA（双槽，校验失败不切槽）或重启；在网后用 STA IP 访问；
- **看门狗**：主循环 1s 一拍喂狗，卡死 >5s panic 重启自恢复
  （`CONFIG_ESP_TASK_WDT_PANIC=y`）。

## 编译与烧录

```bash
cd blink
idf.py set-target esp32s3   # 首次
idf.py build
idf.py -p COMx flash monitor

# 或经 serialtap 代理刷机（板子被守护进程占用时推荐）
serialtap flash <设备正则> build/blink.bin@0x10000
```

本机 Git Bash 直接构建（export 脚本拒绝 MSys，用缓存工具链）：

```bash
IDF_PATH=~/esp/.espressif/v6.0/esp-idf
ROMS=$(ls -d /c/Espressif/tools/esp-rom-elfs/*/ | head -1)
PATH="/c/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:\
/c/Espressif/tools/python/v6.0/venv/Scripts:\
/c/Espressif/tools/ninja/1.12.1:/c/Espressif/tools/cmake/4.0.3/bin:$PATH" \
  cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=$IDF_PATH/tools/cmake/toolchain-esp32s3.cmake
ninja -C build
```

## 硬件占用

| 外设 | GPIO |
|------|------|
| WS2812 | 21 |
| BOOT 键 | 0 |

其余全部空余（WiFi/OTA 走射频与 flash，不占 GPIO；分区表已换 OTA 双槽，
app 偏移 0x20000，见 `partitions.csv`）。板子硬件全貌见
[../README.zh.md](../README.zh.md)。
