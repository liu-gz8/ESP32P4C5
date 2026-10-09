# EMBEDDED_CONTEXT.md — ESP32-P4C5 机器人感知端

最后更新:2026-09-27

## 1. 平台(板卡型号 / SoC / 硬件 revision)

- 板卡型号:OSPTEK(鱼鹰光电)ESP32-P4-Module 基础底板(4 英寸屏版本,含 3D 外壳:ESP32-P4C5-Module-Enclosure-4inch-V1.0)
- SoC:ESP32-P4(CV 版核心板:ESP32P4C5;另有 ESP32P4C6 变体)
- 硬件 revision:底板 V1.5(2026-08-28);核心板芯片 revision v3.2、eco7

## 2. 运算单元(CPU / MCU / GPU / NPU / DSP / 其他加速器)

| 单元 | 型号 / 架构 | 规格(频率 / 核心数 / TOPS 等) | 来源 |
|---|---|---|---|
| CPU | ESP32-P4,RISC-V 双核 | 400 MHz(启动日志 `cpu freq: 400000000 Hz`) | 启动日志 |
| 无线协处理器 | ESP32-C5(通过 SDIO + ESP-Hosted 从机模式) | 2.4/5GHz Wi-Fi 6 + BLE 5 + 802.15.4 | 启动日志「Identified slave [esp32c5]」 |
| 图像加速 | ISP / PPA / 2D-DMA / H.264 编码器 / JPEG 编解码器 | 规格待实测 | ESP-IDF 组件列表 + 模组规格书 |
| NPU | **无独立 NPU**;AI 推理靠 RISC-V 双核 + ESP-DL 的 SIMD 指令 | 算力待实测(EC-01) | 工程推论 |

## 3. 内存与储存(RAM / flash / 储存 / 阶层 / 可用容量)

| 项目 | 总量 | 可用容量 | 阶层 / 备注 | 来源 |
|---|---|---|---|---|
| PSRAM | 32 MB | 未知 | 封装内叠封,200 MHz,AP Memory,16-bit | 启动日志「Found 32MB PSRAM device」 |
| 内部 SRAM | 768 KB HP L2MEM + 32 KB LPSRAM + 128 KB HPROM | 未知 | 放栈 / 队列 / 小结构体 | 模组规格书 |
| Flash | 16 MB | 未知 | Boya(建议开 `SPI_FLASH_SUPPORT_BOYA_CHIP`) | 启动日志「SPI Flash Size : 16MB」 |
| TF 卡 | 支持 4-bit SD | — | 板上卡座 | 原理图 |

## 4. I/O 与周边(摄影机 / 显示 / 网络 / 传感器)

| 周边 | 接口 | 规格 / 限制 | 已验证? |
|---|---|---|---|
| 摄像头 SC2336 | MIPI-CSI 2-lane(24P FPC) | 1920×1080@30fps,默认 1280×720;AVDD 2.8V / DOVDD 1.8V / DVDD 1.5V,**AVDD 上限 3.4V 超出会损坏** | 是(厂商标识 PID=0xCB3A) |
| 显示屏 ST7102 | MIPI-DSI 2-lane(30P FPC) | 480×480 incell;背光升压 SY7200,使能 = GPIO20 | 是 |
| 触摸 ST7123 | I2C(incell) | 最多 5 点,固件 1.71.1.5 | 是 |
| 调试串口 CH343 | USB-TTL | P4 TX=GPIO38 / P4 RX=GPIO37,115200 8N1 | 是(COM6 已枚举) |
| 音频 ES8311 + NS4150B | I2S0 + I2C + 功放使能 | 单麦 LMA3729 + 3W D 类功放 | 否 |
| 板内 I2C 总线 | I2C,SDA=GPIO7 / SCL=GPIO8 | 实测挂 4 个器件:0x18 / 0x28 / 0x30 / 0x55 | 是(已扫描) |
| P4↔C5 链路 | SDIO 4-bit | CLK 18 / CMD 19 / D0-D3 14-17 / Slave_Reset 54 | 是(ESP-Hosted 已建立) |

## 5. 软件堆栈(OS / BSP / SDK / 工具链,一律含版本)

| 元件 | 名称 | 版本 | 来源 |
|---|---|---|---|
| SDK | ESP-IDF | v6.1.0(路径 `D:\ESP-IDF\.espressif\v6.1\esp-idf`) | `build/config.env` |
| 工具链 | riscv32-esp-elf-gcc | GNU 15.2.0(esp-15.2.0_20251204) | 构建输出 |
| Python 环境 | 自建 venv | 3.11.7(`D:\ESP32P4C5\.tools\idf_py_venv`) | 本工作区修复 |
| 烧录工具 | esptool | 5.4.0 | IDF venv |
| AI 框架 | ESP-DL | 3.3.11(本地组件缓存) | `%LOCALAPPDATA%\Espressif\ComponentManager\Cache` |
| 模型库 | pedestrian_detect 0.3.2 / human_face_detect 0.4.2 / human_face_recognition 0.3.2 | — | 同上 |

## 6. Sample 盘点(可用 sample 与各自涵盖的硬件路径)

| Sample | 路径 | 涵盖的硬件路径 | 建置状态 |
|---|---|---|---|
| ESP-IDF MIPI-CSI 示例 | `$IDF_PATH/examples/peripherals/camera/**` | 摄像头采集 | 未试 |
| ESP-IDF MIPI-DSI 示例 | `$IDF_PATH/examples/peripherals/lcd/**` | 屏刷新 | 未试 |
| esp-dl 视觉示例 | 组件缓存 `espressif__esp-dl_3.3.11` | 推理 | 未试 |
| 厂商 demo 固件 | 板上现有固件 `osptek-p4c5-wifi-camera-test`(源码本机没有) | 摄像头 + 屏 + 触摸 + Wi-Fi/BLE 全路径 | 已运行,但**本机无源码** |

## 7. 目标工作负载与产品限制

- 工作负载:人员检测与跟踪、障碍物检测、人脸识别、手势识别、声源方向、表情显示与触摸、720p 视频上行
- 产品限制:推理 ≥15fps、推理延迟 ≤30ms、端到端 ≤150ms、跟随用 bearing ±0.5°(距离只做分档)
- 优先级:安全相关的能力位(避障)不可退让;人脸 / 手势可后置

## 8. 已知结论(每条:结论 | 证据标记 | 日期 | 来源)

- 实物是集成底板,P4↔C5 走 SDIO + ESP-Hosted,不是文档 02/07 假设的 SPI | 已确认 | 2026-09-27 | 启动日志 `build/log/idf_py_stdout_output_21396` |
- 板上 PSRAM 32MB / Flash 16MB | 已确认 | 2026-09-27 | 同上 |
- 摄像头 = SC2336,屏 = ST7102,触摸 = ST7123,背光使能 = GPIO20,调试串口 = GPIO37/38 | 已确认 | 2026-09-27 | 同上 |
- 文档承诺的 `include/robot_link.h` 与 `tools/robot_link_ref.py` 在工作区内并不存在 | 已确认 | 2026-09-27 | 工作区文件系统检查 |
- `idf build` 可从干净目录完整编译通过,产出 192KB 镜像 | 已确认 | 2026-09-27 | 本工作区实跑 |
- AI 框架与检测模型在本地组件缓存中可用,可离线加入工程 | 已确认 | 2026-09-27 | 组件缓存目录清单 |
- 屏 / 触摸 / I2S 的剩余 GPIO 编号尚未确认 | 数据不足 | 2026-09-27 | 原理图 PDF 文本提取不完整 |
- 当前固件未开 PSRAM,Flash size 配置为 2MB(与实物不符) | 已确认 | 2026-09-27 | `sdkconfig` |
- 板级配置已修正并验证:PSRAM 开(HEX/200MHz)、Flash 16MB、Boya 支持、FreeRTOS 1000Hz、自定义分区表(factory 4M + storage 12224K) | 已确认 | 2026-09-27 | `sdkconfig.defaults` + `partitions.csv`,构建输出显示分区表生效 |
- 板子 USB 连接不稳定:read-flash 读到约 2MB 时掉线,随后 COM6 从设备管理器消失 | 已确认 | 2026-09-27 | esptool 报 `GetOverlappedResult failed (PermissionError 13)` |
- ESP-IDF 自带 `examples/peripherals/camera/mipi_isp_dsi`(MIPI 摄像头 → ISP → MIPI 屏),官方支持 SC2336 | 已确认 | 2026-09-27 | `$IDF_PATH/examples/peripherals/camera/mipi_isp_dsi/sdkconfig.defaults` |
- 厂商 demo 经 8 处 IDF 6.1 适配后,**在 IDF 6.1 上编译、烧写、运行全部成功**:屏(ST7102 回 ID)、触摸(ST7123)、摄像头(SC2336 PID=0xcb3a)、V4L2 采集、LVGL 全部工作 | 已确认 | 2026-09-27 | `backup/log_osptek_demo2.txt` |
- 关键坑:`MIPI_DSI_PHY_CLK_SRC_DEFAULT` 在 IDF 6.1 已改为指向 legacy 值(非 DSI LL 接受值),会导致 `esp_lcd_new_dsi_bus` 内 `abort()`;须改用 `MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT` | 已确认 | 2026-09-27 | addr2line 反查 + `soc/esp32p4/include/soc/clk_tree_defs.h:489` |
- 相机通路可用 `esp_video 2.5.0 + esp_ipa 2.4.0 + esp_cam_sensor 2.6.0`(6.1 时代版本),在 IDF 6.1 上无需改源码 | 已确认 | 2026-09-27 | 探针构建与运行 |
- **自研感知端固件(components/cam)在 IDF 6.1 上编译并运行成功**:SC2336 识别正常,1280x720 RGB565 出流,2×1800KB PSRAM 缓冲 | 已确认 | 2026-09-27 | `backup/log_percept_step4.txt` |
- 采集性能:连续 120 秒共 4350 帧,**0 失败**,平均 30.0 fps(最低 29.9 / 最高 30.0),PSRAM 空闲恒定 29162 KB | 已确认 | 2026-09-27 | `backup/log_soak_120s.txt` |
- 传感器模式从 1280×720 改为 **800×800** 后,2m 距离的行人检测置信度达 70~80%;改之前 1280×720 拉伸成 1:1 时几乎只能在很近处检出 | 已确认 | 2026-09-29 | 上板实测(屏上目视 + 串口置信度) |
- 相机镜头为**固定焦距,最佳拍摄距离 80cm**,按 f=4.2mm/F2.0 估算景深约 0.63~1.09m;镜头畸变 <1% | 已确认(规格书)/ 估算(景深) | 2026-09-29 | `doc/datasheet/ESP32-P4-Camera.pdf` |
- **MIPI-DSI 显示已在自研工程中打通**:面板回读 ID `80A0FB`,实时画面 + 检测框叠加正常 | 已确认 | 2026-09-29 | 上板实测 |
- 显示帧缓冲实测地址 `0x48000ac0`(≥0x48000000,落在 **PSRAM**)→ 刷屏前**必须**做 `esp_cache_msync(C2M)` 回写 | 已确认 | 2026-09-29 | `components/disp/disp.c` 启动日志 |
- 16MB 的出厂固件备份中含**硬编码的 WiFi SSID 与密码**(明文),该文件不可公开 | 已确认 | 2026-09-29 | 对 `backup/vendor_demo_16MB.bin` 的字符串扫描 |
- 板上 PSRAM 实测 32768 KB(堆可用 32765 KB)、Flash 实测 16384 KB,Boya 芯片被正确识别 | 已确认 | 2026-09-27 | 自检固件 `backup/log_boot_selftest.txt` |
- 板上分区表实测为 nvs@0x9000 / phy_init@0xf000 / factory@0x10000(4M) / storage@0x410000(12224K),与 `partitions.csv` 一致 | 已确认 | 2026-09-27 | 同上 |
- 板内 I2C(GPIO7/8)上有 4 个器件:0x18、0x28、0x30、0x55 | 已确认 | 2026-09-27 | 同上 |
- 0x18 与 0x30 分别是 ES8311 音频 codec 与 SC2336 摄像头的 SCCB 地址(厂商 Kconfig 的 SCCB 引脚正是 GPIO7/8,与扫描结果一致) | 已确认 | 2026-09-27 | 厂商仓库 `esp32-p4c5-module-dev-board` + `camera-mipi-csi-sc2336` |
- 摄像头 SCCB 与触摸/音频**共用同一条 I2C 总线**,与文档 02 §3.1「不要共用」的建议相反 | 已确认 | 2026-09-27 | 同上 |
- 厂商已在 Gitee(gitee.com/osptek)公开本板全部资料:开发板、SC2336 摄像头、ST7102 屏、核心板,共 80 个仓库,均为 MIT 授权 | 已确认 | 2026-09-27 | 已克隆到 `reference/osptek/` |
- MIPI-CSI 相机引脚:SCCB SCL=GPIO8 / SDA=GPIO7,SC2336 RESET=GPIO26,PWDN 未用;传感器模式 RAW8 1280x720@30fps | 已确认 | 2026-09-27 | 厂商 `main/Kconfig.projbuild` + `sdkconfig.defaults` |
- 厂商相机通路用 esp_video(V4L2)+esp_ipa+esp_cam_sensor 0.9.0,该组合面向 IDF 6.0.x,**在 IDF 6.1 上有 3 处不兼容** | 已确认 | 2026-09-27 | 探针工程 `probe/osptek-camera-demo` 构建失败记录 |
- 内部 SRAM 堆 576 KB(最大连续块 488 KB)、DMA 可用堆 538 KB | 已确认 | 2026-09-27 | 同上 |

## 9. 未解问题与风险

- 屏 / 触摸 / I2S / TF 卡的具体 GPIO — 影响:板级配置与驱动初始化;取得方式:看原理图原图或厂商例程
- P4 板内 I2C 是否摄像头 SCCB 与触摸共用同一条总线(GPIO7/8) — 影响:能否独立排障;取得方式:实测 / 原理图
- 原厂 demo 固件源码 — 影响:能否复用其已验证的初始化序列;取得方式:`github.com/osptk`(需联网)
- 推理算力实测(EC-01) — 影响:模型分辨率与槽位数量;取得方式:上板跑 pedestrian_detect 计时
- 整机端到端延迟与 PSRAM 带宽(EC-07) — 影响:采集 / 显示 / 编码能否同时开

## 10. 已验证流程(建置 / 部署 / 烧录 / 复原)

| 流程 | 状态 | 命令 / 步骤摘要 | 验证方式与日期 |
|---|---|---|---|
| 建置 | 已验证 | `. D:\ESP32P4C5\tools\idf.ps1; idf build` | 产出 `build/ESP32-P4.bin` 192KB,2026-09-27 |
| 烧录 | 已验证 | `. tools\idf.ps1; idf -p COM6 flash` | 三段写回均通过哈希校验,自检固件已运行,2026-09-27 |
| 复原 | 已验证(备份)/未实跑 | `tools/backup_flash.ps1` 分块备份 16MB;还原用 `esptool write-flash 0x0 backup\vendor_demo_16MB.bin` | 备份 sha256 `5B1F40EE70DAE0BB590294BB94EBE5182C88C3CEADB307A78F0ABB4A7B1F4DB7`,结构经 `tools/verify_flash_backup.py` 校验(分区表与启动日志一致);**还原本身尚未实跑过** |
| 抓开机日志 | 已验证 | `python tools\capture_boot.py --port COM6 --seconds 12 --out boot.log` | 先开串口再拉 RTS 复位,能抓到引导第一行,2026-09-27 |
| 自研固件构建 | 已验证 | `. tools\idf.ps1; idf build`(工程 `ESP32P4/ESP32-P4`) | 产出 `ESP32-P4.bin` 555KB,应用区剩 87%,2026-09-27 |
| 自研固件烧写+采集验证 | 已验证 | `esptool write-flash 0x2000/0x9000/0x20000` 后抓串口 | 30fps 稳定 120 秒、0 失败,详见 `backup/log_soak_120s.txt` |

## 11. 决策纪录(每条:决策 | 理由 | 日期)

- 自建 IDF Python venv 而不改动 `C:\Espressif` | 原 venv 指向已卸载的 Python,机器上原有目录不动、不联网即可修复 | 2026-09-27
- 先备份板上原厂固件再烧写 | 原厂 demo 是唯一可运行的摄像头+屏参考,丢了无法本地恢复 | 2026-09-27
