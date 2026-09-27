# AGENTS.md — env-station（工程级约定）

项目级工程笔记：改代码前先读。硬件接线/器件参数/历史排障全过程见
[README](README.zh.md)（中文，最全），用户视角见其 §当前形态。

## 硬约束

| 项 | 值 | 红线 |
|----|-----|------|
| ESP-IDF | v6.0 | Xtensa S3 工具链 |
| Flash/分区 | 4MB，`partitions.csv` 双槽 OTA | app 偏移 **0x20000**（换表首次刷分区表+app） |
| PSRAM | **`CONFIG_SPIRAM=n`** | 本板 Octal 初始化挂死（详见根 README）；变体除外 |
| DHT22 | IO3（RMT 驱动） | strapping 脚，勿配常态输出低 |
| OLED | SSD1306 128×64，I2C SDA=IO6 SCL=IO5 | 单色自绘 UI，**不上 LVGL**（小屏负资产） |
| 看门狗 | 主循环 + DHT 任务挂狗，5s | panic 必须走 `CONFIG_ESP_TASK_WDT_PANIC=y` |

## 模块地图

- `main.c` — 入口/主循环/看门狗/控制台命令（`wifi`/`cal`/`pres?` 等）；
- `dht.c` — RMT 单总线解码（DHT11/22 自动分辨）；
- `ssd1306.c` — OLED 自绘三页 UI（温湿度/诊断/CSI）；
- `csi.c` — CSI 采集 + 本地在场估计（幅度 EWMA 双阈值迟滞，`pres?` 看值）；
- `app_web.c/.h` — 维护页 :80（配网/OTA/重启），APSTA 救援热点
  （`env-station`/`12345678` → 192.168.4.1），OTA 校验失败不切槽；
- `app_disc.c` — WFP v3 发现：UDP :7789 入网播报 + `wfp_probe` 应答 +
  hello 握手 + 30s 判活（超时回本地自治 `#PRES`）；
- `wfp_tcp.c` — WFP TCP 遥测宿（与 USB 串口双宿输出、双通道收命令）；
- `app_probe.c/.h` + `probe_types.h` + `probe_http/tcp/dns/icmp/ws.c` —
  网络探测能力（esp32-blackbox 移植，v3.0）：探测实现与上游逐字一致
  （bug 修复从上游整文件覆盖拷回）；配置 NVS `probe/cfg`（JSON ≤3.5KB）；
  指标端点在 app_web（:80 /metrics、/probe、/api/probe）；控制台
  `probe?` / `probe run <name>` / `probe reset`；探测任务不挂 TWDT。

## 构建变体（多套 sdkconfig 共存）

根 `sdkconfig` 是生成物（已 gitignore）。改 `sdkconfig.defaults` 后必须
`rm -f sdkconfig` 再重新 cmake，否则被静默遮蔽（血泪教训，README 有全程）。
变体构建用 `-DSDKCONFIG` 指到各自目录，互不影响：

```bash
cmake -G Ninja -B build-nopsram -DSDKCONFIG=build-nopsram/sdkconfig \
  -DCMAKE_TOOLCHAIN_FILE=$IDF_PATH/tools/cmake/toolchain-esp32s3.cmake
```

现有变体：`sdkconfig.nopsram`（默认）/ `sdkconfig.psram40` /
`sdkconfig.quad`（未验证）。

## RMT 读 DHT 四坑（复用必读）

1. `mem_block_symbols` ≥ `SOC_RMT_MEM_WORDS_PER_CHANNEL`（S3=48）且偶数；
2. `signal_range_min_ns` < 3187（误配 15000 → RX 永不武装、全超时）；
3. `signal_range_max_ns` < 32767000；
4. 起始脉冲**别用 RMT TX**（推挽+空闲低会把数据线按死，传感器哑）——
   GPIO 开漏拉低 20ms 再切输入。解析：释放伪影多一枚 ~30us 高电平，
   nh=42 丢前 2、nh=41 丢前 1。

## 刷机与链路

- USB 刷机走分区表+app 两件（偏移见上）；长运行态一发即中；
- 板端维护页 OTA：上传整份 app bin，esp_ota_end 校验失败不切槽（无损）；
- 平台链路：USB 串口与 WFP TCP 双宿（遥测双发、命令双收、判活任一续链）；
  阈值学习 `sense_cfg` 落 NVS（掉线仍生效）。

## CI / 提交

与 blink 同规矩：`idf.py build` 必须过，conventional commits
（`feat: / fix: / docs: / chore:`）。
