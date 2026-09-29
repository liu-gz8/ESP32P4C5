/*
 * disp —— MIPI-DSI 显示组件(感知端 ⑦输出 的一部分)
 *
 * 板级信息(已确认):
 *   屏:ST7102 驱动 IC,480x480,2-lane MIPI-DSI,incell 触摸
 *   MIPI PHY LDO:通道 3,2500 mV
 *   面板 ID:读回应为 0x80A0FB
 *
 * 本组件只做"把一块 480x480 RGB565 画布显示到屏上",不引入 LVGL。
 * 需要更复杂的 UI(表情动画)时再评估是否引入 —— 那会显著增加 flash 占用。
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISP_H_RES   480
#define DISP_V_RES   480

/**
 * @brief 初始化 MIPI-DSI 面板
 *
 * 成功后可用 disp_get_framebuffer() 拿到画布。
 */
esp_err_t disp_init(void);

/**
 * @brief 取面板帧缓冲(480x480 RGB565,驱动分配)
 *
 * 直接往这块内存里写像素即可,DPI 硬件会持续扫描输出。
 */
uint16_t *disp_get_framebuffer(void);

/**
 * @brief 提交一帧
 *
 * CPU 写完帧缓冲后必须调用(缓存回写,否则屏上可能是旧内容)。
 * 这是相机那条 cache 通路的镜像:相机是 DMA写->CPU读(需要 M2C),
 * 显示是 CPU写->DMA读(需要 C2M)。
 */
esp_err_t disp_flush(void);

#ifdef __cplusplus
}
#endif
