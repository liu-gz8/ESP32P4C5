/*
 * cam —— MIPI-CSI 摄像头采集组件(感知端 ①采集 段)
 *
 * 职责:把板上的 SC2336 拉起来,对外提供"取一帧 / 还一帧"的最小接口。
 * 不负责前处理、不负责推理,保持单一职责(见文档 04 §4.1 组件划分)。
 *
 * 实现要点:
 *   - 底层用 esp_video 的 V4L2 设备接口(/dev/video0),ISP 输出 RGB565
 *   - 帧缓冲全部在 PSRAM,且按 cache 行对齐(CPU 读取前必须 msync)
 *   - 板内 I2C(GPIO7/8)同时挂着触摸与音频,这里由 esp_video 自建 SCCB 总线
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 打开成功后确定的采集参数 */
typedef struct {
    uint32_t width;         /*!< 帧宽(像素) */
    uint32_t height;        /*!< 帧高(像素) */
    uint32_t pixel_format;  /*!< V4L2 fourcc */
    uint32_t buffer_count;  /*!< 环形缓冲个数 */
    size_t   buffer_size;   /*!< 单个缓冲字节数 */
} cam_info_t;

/** 运行统计,用于文档 09 §2.3 要求的可观测性 */
typedef struct {
    uint32_t frames;            /*!< 成功取到的帧数 */
    uint32_t timeouts;          /*!< 等待帧超时次数 */
    uint32_t failures;          /*!< DQBUF/QBUF 失败次数 */
    uint32_t last_frame_bytes;  /*!< 最近一帧的实际字节数 */
} cam_stats_t;

/**
 * 一帧的所有权凭据
 *
 * 取帧时拿到,归还时交回。之所以要带 index,是为了支持"同时持有两帧":
 * 流水线里采集任务可能一边拿着刚取到的新帧,一边把槽位里的旧帧还回去 ——
 * 只靠内部单一状态是区分不了这两帧的。
 */
typedef struct {
    void  *data;   /*!< 帧数据(PSRAM),CPU 可直接读 */
    size_t len;    /*!< 帧实际字节数 */
    int    index;  /*!< 驱动缓冲号,归还时必须原样交回 */
} cam_frame_t;

/**
 * @brief 初始化摄像头硬件并开始出流
 *
 * 成功返回后即可调用 cam_acquire_frame() 取帧。
 */
esp_err_t cam_init(void);

/** @brief 获取采集参数(cam_init 成功后有效) */
const cam_info_t *cam_get_info(void);

/** @brief 获取运行统计 */
void cam_get_stats(cam_stats_t *out);

/**
 * @brief 取一帧(阻塞直到有帧或超时)
 *
 * @param[out] out         取到的帧(含数据指针、长度、缓冲号)
 * @param[in]  timeout_ms  超时毫秒数(当前未实现,见下方 note)
 *
 * @note 取到帧后必须用 cam_release_frame() 把缓冲还给驱动,否则缓冲耗尽、
 *       出流会停住。
 *
 * @note 归还时**必须用同一个 cam_frame_t**。允许同时持有多个帧(上限为
 *       CAM_BUFFER_COUNT),归还顺序不限 —— 这正是流水线"丢最旧保最新"
 *       所需要的能力。
 *
 * @note 当前 VIDIOC_DQBUF 是阻塞调用,尚未按 timeout_ms 做超时。
 *       等 policy 层需要"丢帧保实时"时再改成 poll/select 实现;
 *       在那之前不要依赖这里的超时语义。
 */
esp_err_t cam_acquire_frame(cam_frame_t *out, uint32_t timeout_ms);

/**
 * @brief 归还一帧缓冲
 *
 * @param[in] frame  由 cam_acquire_frame() 返回的那个帧(原样传回)
 */
esp_err_t cam_release_frame(const cam_frame_t *frame);

#ifdef __cplusplus
}
#endif
