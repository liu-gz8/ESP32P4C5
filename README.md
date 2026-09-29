# ESP32-P4C5 机器人感知端

基于 **ESP32-P4 + ESP32-C5** 模组的机器人视觉感知端固件。跑通了从**摄像头采集 → 边缘推理 → 屏幕显示**的完整链路,并记录了大量实测数据。

> 目标平台:OSPTEK(鱼鹰光电)ESP32-P4C5 模组基础底板(4 英寸屏版本)
> 开发环境:ESP-IDF **v6.1.0** / RISC-V 工具链 esp-15.2.0

---

## 一、已实现能力(带实测数据)

| 能力 | 实测结果 | 证据 |
|---|---|---|
| **MIPI-CSI 摄像头采集** | SC2336,1280×720 RGB565@30fps,**连续 120 秒 4350 帧、0 丢帧** | `log_soak_120s.txt` |
| 采集模式优化 | 切换到 **800×800** 方形模式消除长宽比畸变后,2m 处行人检测置信度 **70~80%** | 上板实测 |
| **边缘推理** | esp-dl + 行人检测模型(int8,输入 224×224),单帧约 **77 ms** | 串口统计 |
| **MIPI-DSI 显示** | ST7102 480×480,面板 ID 回读 `80A0FB`,实时画面 + 检测框叠加 | 上板实测 |
| 全速率显示 | 显示帧缓冲直写 + 缓存回写(C2M) | `components/disp` |

> 早期版本的详细日志在 `docs/工作日志/`(包含一次运行时 `abort()` 的完整定位过程)。

---

## 二、硬件平台

| 部件 | 型号 / 参数 |
|---|---|
| 主控 | ESP32-P4,双核 RISC-V @400MHz,chip rev v3.2 |
| 内存 | **32MB PSRAM**(200MHz)+ 768KB 内部 SRAM |
| 存储 | 16MB Flash(Boya) |
| 无线协处理器 | ESP32-C5,经 **SDIO + ESP-Hosted** 挂在 P4 上(不是 SPI) |
| 摄像头 | SC2336,MIPI-CSI 2-lane,固定焦距(最佳距离 80cm) |
| 显示屏 | ST7102,480×480,MIPI-DSI 2-lane,incell 触摸(ST7123) |
| 音频 | ES8311 codec + NS4150B 功放 + 麦克风 |

板内 I2C(GPIO7/8)上挂 4 个器件:`0x18`(ES8311)、`0x28`、`0x30`(SC2336 SCCB)、`0x55`。

---

## 三、架构与目录

```
.
├── ESP32P4/
│   ├── ESP32-P4/                  # 感知端固件(ESP-IDF 工程)
│   │   ├── main/                  # 应用入口:取帧 → 推理 → 缩放 → 显示
│   │   ├── components/
│   │   │   ├── cam/               # ① 采集:MIPI-CSI + esp_video(V4L2)
│   │   │   ├── infer/             # ③④ 推理与后处理:esp-dl 行人检测
│   │   │   ├── disp/              # ⑦ 显示:MIPI-DSI 面板初始化与刷屏
│   │   │   └── esp_lcd_st7102/    # ST7102 面板驱动(第三方,Apache-2.0)
│   │   ├── partitions.csv         # 8MB 应用区 + 8MB 数据区
│   │   └── sdkconfig.defaults     # 全部板级配置(可完整复现)
│   └── doc/
│       └── rules/                 # 架构与接口设计文档(00~10)
├── docs/
│   └── 工作日志/                   # 开发过程记录(含问题定位过程)
├── tools/                         # 开发辅助脚本
└── EMBEDDED_CONTEXT.md            # 项目认知摘要(平台/结论/待验证项)
```

**设计原则**

- 每个组件单一职责,只通过头文件交互(`cam` 只负责取帧,`infer` 只负责推理,`disp` 只负责显示)
- 帧缓冲全部在 PSRAM,并做缓存一致性处理
- 配置全部落在 `sdkconfig.defaults`,仓库不提交生成物

---

## 四、构建与烧录

### 4.1 前置条件

- ESP-IDF v6.1.0(需同时支持 ESP32-P4)
- 工具链:riscv32-esp-elf(ESP-IDF 自带)

### 4.2 构建

```bash
cd ESP32P4/ESP32-P4
idf.py set-target esp32p4      # 首次
idf.py build
```

### 4.3 烧录

```bash
idf.py -p <PORT> flash monitor
```

分区表偏移为 **0x9000**(不是默认的 0x8000):bootloader 在 IDF 6.1 下约 0x6140 字节,放不进 0x8000 起的空间。

### 4.4 依赖

外部组件由组件管理器按 `main/idf_component.yml` 自动获取:

| 组件 | 版本 | 用途 |
|---|---|---|
| `espressif/esp_video` | 2.5.0 | 摄像头 V4L2 设备接口 |
| `espressif/esp_cam_sensor` | 2.6.0 | SC2336 传感器驱动 |
| `espressif/esp_ipa` | 2.4.0 | ISP 算法管线 |
| `espressif/esp-dl` | 3.3.12 | 推理框架 |
| `espressif/pedestrian_detect` | 0.3.2 | 行人检测模型(224×224×3,int8) |

---

## 五、开发环境要点:ESP-IDF 6.1 适配

本项目使用的模组厂商示例工程面向 **ESP-IDF 6.0.2**,迁移到 6.1 遇到若干破坏性变更。完整记录见 `ESP32P4/doc/rules/10_实际硬件与开发环境记录.md`,其中三个值得单独提出:

**1. 依赖版本不要锁死厂商指定的旧版本。**
厂商锁定的 `esp_video 0.8.*` 使用了 6.1 已删除的枚举(`CAM_CTLR_COLOR_YUV422`);把约束放开到最新版,组件管理器解析到 2.5.0,**源码无需修改即可编译**。

**2. 兼容别名可能导致运行时崩溃(最难查的一类)。**
IDF 6.1 保留了旧宏 `MIPI_DSI_PHY_CLK_SRC_DEFAULT`,但把它重定义成了 `..._DEFAULT_LEGACY`,而 DSI 的 HAL 层只接受新的枚举值,于是命中 `switch` 的 `default: abort()`。
**症状是板子反复重启**,代码可编译、无警告、故障点在第三方驱动内部。定位方法:`addr2line` 反查 `abort()` 的 PC 与 `MEPC`,注意输出里的 `(inlined by)` 行。

**3. 结构性字段改名。**
`esp_lcd_panel_dev_config_t.color_space` → `rgb_ele_order`;`esp_lcd_dpi_panel_config_t.pixel_format` → `in_color_format` + `out_color_format`;`.flags.use_dma2d` → `esp_lcd_dpi_panel_enable_dma2d()`。

---

## 六、第三方代码与许可

| 部分 | 来源 | 许可 |
|---|---|---|
| `components/esp_lcd_st7102/` | 显示模组厂商公开的驱动 | 文件内标注 SPDX `Apache-2.0` |
| 其余 `components/` 与 `main/` | 本项目自行编写 | 见 `LICENSE` |

**未包含在本仓库的内容**(体积、版权或安全原因):

- 模组厂商的原始 datasheet、原理图、结构图 PDF(请向模组厂商索取)
- 出厂 demo 的固件备份(其中含硬编码的 WiFi 凭据,不适合公开)
- 厂商在 Gitee 上公开的参考工程(请访问原仓库)

---

## 七、相关文档

| 文档 | 内容 |
|---|---|
| [`doc/rules/00`](ESP32P4/doc/rules/00_交付说明与阅读顺序.md) | 文档索引与阅读顺序 |
| [`doc/rules/01`](ESP32P4/doc/rules/01_系统架构设计.md) | 三节点系统架构、权威边界、数据流 |
| [`doc/rules/09`](ESP32P4/doc/rules/09_推理端实施方案.md) | 感知端推理方案与实施计划 |
| [`doc/rules/10`](ESP32P4/doc/rules/10_实际硬件与开发环境记录.md) | **实机确认的硬件参数 + 环境适配记录** |
| [`EMBEDDED_CONTEXT.md`](EMBEDDED_CONTEXT.md) | 平台参数、已验证结论、待验证项 |
| [`docs/工作日志/`](docs/工作日志/) | 开发过程与问题定位记录 |
