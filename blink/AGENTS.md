# AGENTS.md — blink（工程级约定）

基线工程：验证这块板的最小闭环（WS2812 / BOOT 键 / 心跳日志 /
USB-Serial-JTAG），也是板端标配能力（维护页/OTA/看门狗）的最简参考实现。
硬件事实见根 [README](../README.zh.md)。

## 硬约束

| 项 | 值 | 说明 |
|----|-----|------|
| ESP-IDF | v6.0 | Xtensa S3 工具链 |
| Flash | 4MB | 分区表 `partitions.csv`：双槽 OTA（ota_0/ota_1），app 偏移 0x20000 |
| WS2812 | GPIO21（板载） | `led_strip` RMT 驱动 |
| BOOT 键 | GPIO0 | 输入上拉，按下为 0 |

## 结构约定（同板项目保持同名同构）

- `main/main.c` — 入口：心跳循环、看门狗、启动自检（heap/PSRAM 打印）；
- `main/app_web.c/.h` — 板端 Web 维护页（:80）：状态 / WiFi 配网 / OTA 上传 /
  重启，前后端一体内嵌单页；自带 APSTA 救援热点（未配网时 `blink-s3`/
  `12345678` → 192.168.4.1），STA 关联后 AP 自动跟随信道；
- 看门狗：主循环 1s 喂狗，5s 超时 panic 重启；**panic 必须走
  `CONFIG_ESP_TASK_WDT_PANIC=y`**（代码里 trigger_panic 会被已建 TWDT 吞掉）。

## 构建与烧录

PowerShell 下 `idf.py set-target esp32s3 && idf.py build`（Git Bash 会被
export 脚本的 MSys 检测拒绝；工程已设 `__CHECK_PYTHON 0` 可绕预检）。
烧录口被串口代理类工具占用时先停代理。OTA 包用整份 app bin
（`build/blink.bin`），从维护页上传即可。

## 同构复用

env-station 的 `app_web.c` 由本文件逐字拷贝后改造（自含 WiFi 版 →
APSTA 双模版）。改维护页协议（/api/status、/api/wifi、/ota）时两处同步，
第三块板验证后抽入共享组件库。
