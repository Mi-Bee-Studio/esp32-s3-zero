# env-station — 环境小站（0.96" OLED + DHT 温湿度 + 左右按钮 + CSI 人体感应）

[English](README.md) | [中文文档](README.zh.md)

**状态（2026-09-26，固件 0.1.0）—— 新增网络探测能力（blackbox 并入），WFP 协议 v3/v3.1 首个实现板**：
UDP :7789 入网播报（零配置被 homepulse 发现自动建会话）+ hello 身份握手 +
PC 链路判活（30s 无平台命令→本地自治）+ 端侧本地在场估计（#PRES 行）+
**增强协议**（PC 在链期间采样学习专属阈值 sense_cfg 下发、NVS 持久）。
v2.5 起板端 Web 维护页（`http://<设备IP>/`，配网/OTA/重启）与看门狗
已常态。温湿度传感器为 **DHT22**（IO3）；CSI 走 homepulse 节点路线
（#S1/#ENV）。历史排查记录见文末。

## 目标形态

- 0.96" OLED（128×64）显示温湿度与在场状态；左右按钮翻页/交互；
- CSI 人体感应两条路线（固件阶段定，可兼得）：
  1. **homepulse 节点**：CSI 原始数据经 USB 串口（serialtap 透传）上 PC，
     DSP 在 homepulse——与 luatos C3 感知终端同构，#S1 协议经验直接复用；
  2. **端侧粗检测**：S3 双核 240MHz 有余量，幅度方差类阈值判在场，
     OLED 直接出结果。

## 硬件清单

| 器件 | 型号 | 数量 | 说明 |
|------|------|------|------|
| 主板 | Waveshare ESP32-S3-Zero | 1 | 板级信息见 [../README.zh.md](../README.zh.md) |
| 显示屏 | 0.96" SSD1306 OLED，128×64 | 1 | I2C 4 针版 |
| 按钮 | 6×6mm 轻触开关（左/右） | 2 | 无源、无极性 |
| 温湿度 | DHT11 或 DHT22 | 1 | 单总线，DATA 接 IO3 |

---

## 器件参数

### 0.96" OLED（SSD1306）

| 参数 | 值 |
|------|-----|
| 分辨率 | 128 × 64（单色） |
| 驱动 IC | SSD1306 |
| 接口 | I2C（4 针，最常见）或 SPI（7~8 针，少数） |
| I2C 地址 | 0x3C（默认）/ 0x3D（少数） |
| 供电 | 3.3 ~ 5V，**本项目统一接 3V3** |
| I2C 速率 | ≤400 kHz |
| 电流 | 全亮 ~10–25 mA |
| 引脚 | GND / VCC / SCL / SDA —— **SDA、SCL 丝印顺序各家不同，认丝印不认位置** |

> 如果你的屏是 SPI 版（针脚里有 DC / CS / RES / BLK 丝印），接线另有一套，
> 见文末「SPI 版 OLED 备用方案」，先别照 I2C 表接线。

### 左右按钮

- 左键 = IO1，右键 = IO2（对称接法，固件里可随意互换）；
- 一脚接 GPIO、另一脚接 GND，GPIO 用内部上拉，**按下读到 0**；
- 消抖交给软件（v0 固件 20ms 双采样，正式版换中断 + 队列）。

### DHT 温湿度传感器（DHT11 vs DHT22）

| 参数 | DHT11 | DHT22 / AM2302 |
|------|-------|----------------|
| 接口 | 单总线（1 根数据） | 单总线（1 根数据） |
| 温度量程 | 0 ~ 50 ℃ | −40 ~ 80 ℃ |
| 温度精度 | ±2 ℃ | ±0.5 ℃ |
| 湿度量程 | 20 ~ 90 %RH | 0 ~ 100 %RH |
| 湿度精度 | ±5 %RH | ±2~5 %RH |
| 最快采样 | 1 次/秒 | 1 次/2 秒 |
| 供电 | 3.3 ~ 5.5 V | 3.3 ~ 6 V |
| 数据格式 | 8bit 整数 RH / T | 16bit，温度带符号 ×0.1 |

- **接法完全相同**：VCC→3V3、DATA→IO3、GND→GND；
- 裸件（4 针无板）DATA 需 4.7~10 kΩ 上拉到 3V3；三针蓝板模块已板载，直接接；
- **分辨型号**：看外观（蓝色小方壳=DHT11，白色大网格大一圈=DHT22）或丝印
  （AM2302=DHT22）。不猜也行：v1 固件先按 DHT22 解码、数值不合理再按 DHT11
  解码，自动分辨；
- 备选方案（当前不用）：SHT30/SHT40 是 I2C 温湿度（0x44，±0.2℃/±2%RH，
  10Hz）——日后若嫌 DHT 慢/糙，直接接到 IO8/9 同一条 I2C 总线即可，驱动比
  DHT 更简单。

---

## 接入主板

### 设计原则

OLED 独占 I2C 总线（IO8/9），DHT 单总线独占 IO3，按钮各占一根——互不冲突；
CSI 走 WiFi 射频，不占任何 GPIO。

### 引脚分配（2026-09-22 宽探测实测后按实际接线修订）

| 功能 | GPIO | 状态 | 说明 |
|------|------|------|------|
| I2C SDA | **IO6** | ✅ 实测（OLED 0x3C 应答） | OLED SDA |
| I2C SCL | **IO5** | ✅ 实测 | OLED SCL |
| 左按钮 | **IO1** | 待按键确认 | 另一脚接 GND，内部上拉 |
| 右按钮 | **IO2** | 待按键确认 | 同上 |
| DHT DATA | **IO3** | 未接/未验 | strapping 注意见下节 |
| （板载）BOOT 键 | IO0 | 板载 | 可客串"第三按钮" |
| （板载）WS2812 | IO21 | 板载 | 本项目暂不占用 |

> 实测备注：接线与最初规划（I2C=IO8/9）不一致，宽探测固件扫出 OLED 实际
> 在 SDA=IO6、SCL=IO5（反序）。后续固件按实际脚写；IO8/9 空出。

### ⚠ IO3 是 strapping 脚（DHT 接这里须知）

- IO3 在复位时被采样，决定 **JTAG 源**：1 = USB-Serial-JTAG（默认，脚上有弱
  上拉），0 = 外部 JTAG 引脚（IO39~42）。**只影响 JTAG 调试的路由，不影响
  USB 串口日志，也不影响 serialtap/esptool 刷机**；
- DHT 的 DATA 线经上拉常态为高 → 上电采样读到 1，正好就是默认状态，无影响；
- 唯一风险窗口：按 RESET 的瞬间 DHT 恰好在通信、把线拉低 → 那一次启动 JTAG
  切到外部引脚。后果轻微（断电重上即恢复），介意的话直接换 IO4/IO5/IO6，
  接线和固件改一个宏即可；
- 固件注意：RMT 读 DHT 是"推低→释放→读"，不要把 IO3 配成常态输出低。

### 接线表

**OLED（I2C 版）**：

| OLED 针 | 接主板 |
|---------|--------|
| GND | GND |
| VCC | 3V3 |
| SCL | IO9 |
| SDA | IO8 |

**DHT11 / DHT22**：

| 传感器针 | 接主板 |
|----------|--------|
| VCC（+） | 3V3 |
| DATA（out） | IO3 |
| GND（−） | GND |

裸件另加：NC 悬空，DATA 与 3V3 之间 4.7~10 kΩ 上拉电阻。

**按钮 ×2**：左键一脚 IO1、右键一脚 IO2，另一脚都接 GND。

```
3V3 ●──┬── OLED VCC
       └── DHT VCC
GND ●──┬── OLED GND
       ├── DHT GND
       └── 左、右按钮的另一脚
IO8 ●── OLED SDA       IO9 ●── OLED SCL
IO1 ●── 左按钮         IO2 ●── 右按钮
IO3 ●── DHT DATA
```

### 电气注意事项

- **全部 3V3 供电**：DHT11/22 在 3.3V 下工作正常；别用 5V 供电，那样 DATA
  高电平是 5V，会打坏 ESP32 的 IO；
- 板上 GND 排孔只有一个，多器件请经面包板地轨汇接到这一个孔；
- I2C 上拉：OLED 模块板载上拉，无需再加（v0 固件同时开了内部上拉兜底）；
- DHT 时序敏感：正式固件用 **RMT 外设**采集，别用 bit-bang；采样间隔
  ≥1s（DHT11）/ ≥2s（DHT22）；
- 供电余量：OLED 全亮 ~25mA + DHT ~1mA + S3 满载（含 WiFi 突发）几百 mA，
  USB 供电完全无忧。

### CSI 人体感应（新增能力）

- **能力**：ESP32-S3 支持 WiFi CSI（`esp_wifi_set_csi`）；
  `sdkconfig.defaults` 已开 `CONFIG_ESP_WIFI_CSI_ENABLED` 预留；
- **激励**：CSI 只在有 WiFi 流量时触发，需要持续激励——C3 项目实测 ping
  网关最稳；本板与激励源关联同一路由即可；
- **链路定位**：走 homepulse 节点路线时，CSI 帧经 USB 串口由 serialtap 透传
  上 PC（面板 :8801 可直接观测字节流），DSP/多径扣除在 homepulse；端侧粗
  检测路线则本板闭环，只上送结果。固件阶段二选一或兼得；
- **摆放**：CSI 对多径极其敏感——位置和朝向固定后别频繁挪动；OLED 与排线
  不要覆盖板载 PCB 天线一端，整机远离金属。

### SPI 版 OLED 备用方案（仅当屏是 7~8 针版）

OLED 走 SPI2，DHT 照旧接 IO3：SCK=IO12、MOSI=IO11、CS=IO10、DC=IO13、
RES=IO5、BLK 悬空或 IO6。确认是 SPI 版后再接，固件按 SPI 面板配置。

---

## 按键与界面

双色屏分区：**黄带 = 状态带**（页名 / WiFi 信号条 / 串流呼吸点 / PC 在链标记），
**蓝区 = 数据**，底行 = 按键提示（动作后 1.5s 让位给 toast 回执）。

三个页面（左键短按轮换）：

| 页 | 内容 |
|----|------|
| **ENV** | 温湿度大字（°C/%）、露点（Magnus）、读数计数 |
| **CSI** | 在场判定大字（IN/OUT，PC 在链用平台判定、否则本地估计）、运动条 + 学习阈值刻度（sense_cfg 下发）、采样率 |
| **SYS** | 运行时长 / 内存 / CSI 累计 / IP / DHT 读数与错误 / 探测成功数 |

按键语法（与 luatos 感知终端同一套）：

| 键 | 作用 |
|----|------|
| L 短按 | 翻页 ENV → CSI → SYS |
| L 长按 1s | 串流开关（全局，toast 回执） |
| R 短按 | 页内主操作：ENV=立即读数 · CSI=串流开关 · SYS=诊断快照进日志 |
| R 长按 1s | 重启进 ROM 下载模式（免拔插刷机自救，不变） |
| L+R 同按 2.5s | 软重启（1.5s 时屏幕提示倒计时） |
| 空闲 10min | 自动熄屏（防烧屏规范，GRAM 保留，采集不受影响）；之后**随机窥视**：每 1-5min 亮 1-60s，随机显示当前页数据或表情动画（眨眼/开心脸）；任意键唤醒（该次按键只唤醒不触发动作） |

## v0 探测固件


行为：

1. 启动打印 heap / PSRAM 容量（验证 Octal PSRAM 配置）；
2. I2C 全段扫描（0x08~0x77），命中 0x3C/0x3D 标注 OLED，0x44/0x45 标注
   I2C 温湿度（SHT 备选），什么都没有则提示查接线；
3. 左按钮（IO1）/右按钮（IO2）状态变化即打印（按下=0 / 松开=1，
   20ms 双采样消抖）；
4. 每 10 秒一条 heartbeat。

本版**未含**：DHT 解码（v1，RMT）与 CSI（感知阶段）。

编译（缓存工具链直连 ninja，已在本机验证通过，产物 ~192KB）：

```bash
cd env-station
export IDF_PATH=~/esp/.espressif/v6.0/esp-idf   # 必须 export，cmake 内部取 $ENV{IDF_PATH}
export PATH="/c/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:\
/c/Espressif/tools/python/v6.0/venv/Scripts:\
/c/Espressif/tools/ninja/1.12.1:/c/Espressif/tools/cmake/4.0.3/bin:$PATH"
cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=$IDF_PATH/tools/cmake/toolchain-esp32s3.cmake
ninja -C build
```

两个 Git Bash 专属的坑（已在本项目内解决，记录备查）：

- **MSys 检测**：`idf_tools.py` 入口拒绝 MSys，且 MSYS 运行时会向原生子进程
  重新注入 `MSYSTEM`，shell 里 `unset` 无效 —— 顶层 CMakeLists 里已用
  `idf_build_set_property(__CHECK_PYTHON 0)` 跳过该预检（依赖本身装在
  `/c/Espressif/tools/python/v6.0/venv`，检查是误伤）；
- **偶发编译器 ICE**：IDF 自带 `esp_lcd` 的 RGB 源文件在 gcc 15.2.0 下极偶发
  内部段错误，重跑一次 `ninja -C build` 即过，与本项目代码无关。

**改过 `sdkconfig.defaults` 后必须 `rm -f sdkconfig` 再重新 cmake**——根目录
`sdkconfig` 优先级高于 defaults，不删则改动被静默遮蔽（本项目实测：改
`SPIRAM=n` 未删 sdkconfig，重建出的仍是 Octal 固件，刷上即挂）。变体构建
（`build-nopsram/`、`build-psram40/`、`build-quad/`）用 `-DSDKCONFIG` 指到各自
构建目录，不受根 sdkconfig 影响。

烧录（板子被 serialtap 占用时走代理；v2.3 起 OTA 双槽分区，**app 偏移
0x20000**，首次换表要刷分区表+应用，bootloader 不变）：

```bash
serialtap flash <设备正则> \
  build/partition_table/partition-table.bin@0x8000 \
  build/env-station.bin@0x20000
```

---

## 下一步

1. 接 OLED + 左右按钮 → 刷 v0：确认 0x3C 与左右键日志；
2. DHT 接 IO3 → v1：RMT 解码（DHT11/22 自动分辨），日志出温湿度；
3. OLED 点亮：esp_lcd + SSD1306 单色自绘 UI（128×64 不上 LVGL——luatos
   项目的 LVGL 教训对这块小屏是负资产），温湿度首屏；
4. CSI 阶段：WiFi 关联 + `esp_wifi_set_csi` 回调采集 → 先打日志验证激励，
   再定 homepulse 节点（复用 #S1 经验）还是端侧粗检测；
5. 全程经 serialtap 面板观测日志与字节流。

---

## 当前排查记录（2026-09-22）

**PSRAM 结论（已定）**：标称 Octal 2MB 与实物不符——`SPIRAM_MODE_OCT`
在 80M **和 40M** 下都 `octal_psram: PSRAM chip is not connected, or wrong
PSRAM line mode` → abort → 77ms boot-abort 循环 → 总线级挂死（软复位无效，
USB 从总线消失，只能断电）。esptool 报芯片 v0.2（2021-03 ROM）、
"Embedded PSRAM 2MB (AP_3v3)"。**项目默认已改 `CONFIG_SPIRAM=n`**
（sdkconfig.defaults），Quad 变体（`build-quad/`）未测。PSRAM 初始化挂死
的板子恢复流程：**普通拔插 USB（不按 BOOT）+ `serialtap/catch-flash.sh`
守窗口脚本**——检测端口"消失→出现"跳变沿后让 esptool 把芯片停进 ROM
下载模式（`--after no-reset`），再代理刷回无 PSRAM 固件，全程自动。

**v1 运行验证（nopsram 构建）**：板子正常启动，`heap≈397KB`，心跳 10s/条
稳定，左右按钮（IO1/IO2）实测工作（IO1 27 次 / IO2 43 次事件）。**OLED 已
定位（宽探测）**：SDA=**IO6**、SCL=**IO5**（0x3C 应答，v1 的 SSD1306 驱动
点亮正常）。**DHT：修复 RMT RX 毛刺滤波参数超限（`signal_range_min_ns`
须 <3187ns，误配 15000ns 导致 RX 从未武装、全部超时）后，带自动扫描的
v1 已确认 IO3/4/7~13 均无应答——传感器本体/接线问题，待查（VCC=3V3、
GND、DATA→IO3；裸件需 4.7~10k 上拉，模块自带）。**

**v1 全链路收官（2026-09-22 21:00）**：OLED（IO6/IO5）点亮显示温湿度；左键翻页
（温湿度/诊断）、右键立即读、**右键长按 1s 重启进 ROM 下载模式**（免拔插刷机
自救）；**DHT22@IO3 周期采集正常**（2.5s/次，型号自动识别，日志带原始字节），
心跳带温湿度。

**v2.2 当前形态（2026-09-22 22:55）**：
- **双色屏 UI**：黄带（行 0-15）= 页标题，蓝区（行 16-63）= 数据；三页循环
  （左键）：温湿度大字 / 诊断 / CSI 状态（含 PC:IN/OUT 在场显示）；
- **温湿度校准**：`cal <toff> <rhoff>`（NVS 持久，`cal?` 查询——DHT22 贴板自热，物理正解是延长线挪开传感器）；
- **CSI/homepulse 互操作**：`#S1-HELLO`+`#S1` 同 luatos 格式；响应
  sense_start（stimulus_hz 调 ping 速率）/sense_stop/sense_status（present
  上屏）。凭据经 `wifi <ssid> <pass>` 串口命令下发（NVS wifi/ssid,pass）；
- **命令行**：驱动 API 读取（fgets 不工作，照抄 luatos 的
  usb_serial_jtag_read_bytes 模式）；右键长按 1s 进下载模式；主循环看门狗 5s。
- **刷机**：v2.x 常驻读串口后长运行态一发即中（serialtap flash 直刷）。

**v2.5 板端 Web 维护页 + OTA + 看门狗（2026-09-24）**：
- **分区表换 OTA 双槽**（`partitions.csv`：ota_0/ota_1 各 1984K，nvs/phy
  位置不变——校准与凭据无损迁移；app 偏移 0x10000 → **0x20000**）；
- **app_web.c（前后端一体，:80）**：内嵌单页（状态卡/配网表单/固件上传
  XHR 进度条/重启）+ esp_http_server 后端（GET /、/api/status、POST
  /api/wifi、/api/reboot、**POST /ota**——流式写备用槽→esp_ota_end 校验
  （失败不切槽，原固件无损）→切槽→延迟 1s 重启）；
- **常开 SoftAP "env-station"/12345678**（APSTA）：未配网/掉线时
  192.168.4.1 永远是救援通道，配网不依赖串口——手机连热点即可下发 WiFi
  凭据并 OTA 刷机；STA 关联后 AP 信道自动跟随，CSI 不受影响；
- **看门狗强化**：主循环（20ms）+ DHT 任务（2.5s）都挂狗，
  `CONFIG_ESP_TASK_WDT_PANIC=y` 让超时真正触发重启（此前代码里的
  trigger_panic 因系统已建 TWDT 返回 INVALID_STATE 被吞，panic 必须走
  sdkconfig 才生效）；
- 同构拷贝：blink（自含 WiFi 版）、blackbox（双服务器版）同日落地，
  luatos 受 2MB flash 限制只加看门狗（详见 [blink/AGENTS.md](../blink/AGENTS.md)（维护页/OTA/看门狗工程约定））。

**v2.6 WFP 协议 v3：零配置发现 + 双向握手 + 本地自治（2026-09-26）**：
- **入网播报（app_disc.c）**：拿到 IP 后每 10s 向 255.255.255.255:7789 广播
  一行 `#HELLO {devid,proto,fw,name,caps,ip,tcp}`；收到 PC 广播
  `{"cmd":"wfp_probe"}` 即时单播应答。homepulse 监听 :7789 即识别**从未
  配置过**的板子并自动建 TCP 会话（静态配置优先，同 IP/同名去重）；
- **hello 握手**：`{"cmd":"hello"}` → `#HELLO {...}`（devid=efuse MAC 派生）；
- **PC 链路判活**：任一平台命令（hello/sense_*/wifi_*）续链，**30s 无命令
  回本地自治**——OLED 在场显示从 `PC:IN/OUT`（平台权威）切 `LOC:IN/OUT`
  （本地估计），并上送 `#PRES {"seq","present","src":"csi-local","m"}`；
- **本地在场估计**：CSI 幅度（|I|+|Q|）帧间差 EWMA，双阈值迟滞（enter>25
  exit<12）+ 防抖计数；暖机 600 帧无动作默认"无人"。阈值观察/调参：
  串口 `pres?`、heartbeat `m=`、/api/status `motion`；
- **Windows 防火墙注意**：PC 侧无入站规则时广播播报被拦——homepulse 的
  网段单播扫描兜底仍可 ≤30s 发现（应答走状态针孔）；加 UDP 7789 入站
  规则后播报路径即时直达。

**v2.7 增强协议（2026-09-26 晚，WFP v3.1）**：PC 在链期间 homepulse 每 5s
`sense_stat` 采样板端运动量 m，对照其 DSP 判定学出**这块板专属双阈值**，
`sense_cfg` 下发（NVS "env"/moten,motex ×0.1 持久，掉线仍生效）——
"接了 PC 就变强，拔了 PC 不回退"。阈值观察：串口 `pres?`、
`/api/status` mot_enter/mot_exit。**双通道并存语义**（协议 §6）：遥测
双宿输出（串口+TCP 各发一份）、命令双通道共收、判活任一通道续链。
原理性上限：纯运动指标对静坐人体无分离度（PC 呼吸类 DSP 兜底）。

**v3.0 网络探测能力（2026-09-26，esp32-blackbox 并入）**：本板资源充足
（OTA 槽余 47%），把 blackbox 能力作为第四能力并入同一固件：
- **探测内核与上游逐字一致**（probe_http/tcp/dns/icmp/ws.c，8 类 prober：
  HTTP/HTTPS/TCP/TCP+TLS/DNS/ICMP/WS/WSS），挂 `app_probe.c` 适配层；
- **配置后端 SPIFFS→NVS**（本板 4MB 被 OTA 双槽占满无 storage 分区；
  namespace `probe`/key `cfg`，JSON ≤3.5KB，schema 与上游完全兼容）；
- **指标端点并到维护页 :80**（不另起 :9090）：`GET /metrics`（Prometheus
  抓取）、`GET /probe?target=X&module=Y[&port=P]`（exporter 兼容按需探测）、
  `GET/POST /api/probe`（配置读取/提交热加载）；维护页新增探测卡片
  （目标状态表 + JSON 编辑器）；
- 控制台：`probe?` / `probe run <name>` / `probe reset`；
- 探测任务不挂 TWDT（主循环 5s 看门狗灵敏度不变；探测自身受模块超时
  1-120s 约束）；WiFi 未关联时空转。出厂默认 4 个标准模块、0 目标
  （家庭设备不主动外联，用 /probe 按需或面板加目标）。

**serialtap 端口泄漏修复（2026-09-22，杂项目反哺中间层）**：proxy 会话 +
pause 后端口永久 busy 的根因是 collectOnce 的 defer 顺序倒置（Close 先于
写入口摘除执行，Windows 重叠 IO 下 Close 静默失败句柄泄漏）。已在
internal/collector/collector.go 修复并回归验证；proxy 用后记得 `-stop`。

**RMT DHT 驱动四处坑（全修，复用注意）**：① `mem_block_symbols` ≥
SOC_RMT_MEM_WORDS_PER_CHANNEL（S3=48）且偶数；② `signal_range_min_ns`<3187；
③ `signal_range_max_ns`<32767000；④ **起始脉冲别用 RMT TX**（推挽+空闲低电平
把数据线常按 GND，传感器永远哑）——用 GPIO 开漏拉低 20ms 再切输入。解析时
释放伪影会多一枚 ~30us 高电平：nh=42 丢前 2、nh=41 丢前 1。

**早期事件**（同日）：出厂固件按 8MB flash 编译在本 4MB 板上无限重启——
刷机前端口"时好时坏"（ERROR 31 等）的根源，非接线问题。

## 硬件占用

| 外设 | GPIO | 状态 |
|------|------|------|
| I2C（OLED） | IO8 / IO9 | 规划 |
| 左 / 右按钮 | IO1 / IO2 | 规划 |
| DHT DATA | IO3 | 规划（strapping 注意） |
| SPI 备用（SPI 版 OLED） | IO5, IO10~13 | 仅 SPI 屏方案 |
| WS2812 / BOOT | IO21 / IO0 | 板载，暂不占用 |
