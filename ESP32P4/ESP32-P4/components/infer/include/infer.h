/*
 * infer —— 推理组件(感知端 ③推理 + ④后处理 段)
 *
 * 对外只暴露 C 接口,内部用 C++ 调 esp-dl(它本身是 C++ 框架)。
 * 这样 main.c 之类的纯 C 代码不用被 C++ 污染,边界清晰。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 单帧最多返回的检测框数,和文档 09 §4.2 的"最大输出框数"约定一致 */
#define INFER_MAX_BOXES 8

/** 一个检测框(坐标已经是原图坐标系) */
typedef struct {
    int16_t  x;           /*!< 左上角 x,像素,原图坐标系 */
    int16_t  y;           /*!< 左上角 y */
    int16_t  w;           /*!< 宽,像素 */
    int16_t  h;           /*!< 高,像素 */
    uint16_t score_pct;   /*!< 置信度,百分数(0-100),方便打印 */
    uint8_t  category;    /*!< 类别编号 */
} infer_box_t;

/** 运行统计(文档 09 §2.3 要求的可观测性) */
typedef struct {
    uint32_t frames;     /*!< 已完成的推理帧数 */
    uint32_t last_us;    /*!< 最近一次推理耗时(微秒,含前处理+后处理) */
    uint32_t min_us;     /*!< 最小耗时 */
    uint32_t max_us;     /*!< 最大耗时 */
} infer_stats_t;

/**
 * @brief 加载模型并初始化推理组件
 *
 * 模型常驻内存(文档 09 §3.3 的反模式清单:禁止运行时从 Flash 反复加载卸载)。
 */
esp_err_t infer_init(void);

/**
 * @brief 对一帧 RGB565 图像做检测
 *
 * @param[in]  rgb565     帧数据(来自 cam_acquire_frame)
 * @param[in]  width      帧宽,**必须传真实值**,后处理用它把框映射回原图
 * @param[in]  height     帧高
 * @param[out] boxes      结果数组
 * @param[in]  max_boxes  boxes 数组容量(最大 INFER_MAX_BOXES)
 * @param[out] out_count  实际检出的框数
 */
esp_err_t infer_run(const uint8_t *rgb565, uint32_t width, uint32_t height,
                    infer_box_t *boxes, size_t max_boxes, size_t *out_count);

/** @brief 读取运行统计 */
const infer_stats_t *infer_get_stats(void);

#ifdef __cplusplus
}
#endif
