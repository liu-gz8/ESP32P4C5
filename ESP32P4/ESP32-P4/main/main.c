/*
 * 感知端 · 步骤 5:单模型推理闭环
 *
 * 数据流(对应文档 09 §2.1 的七段):
 *   ① 采集(cam) → ③④ 推理+后处理(infer) → ⑦ 输出(打日志)
 *
 * 目前还没有 policy / track / link,先把这个最小闭环跑通并测出真实耗时。
 *
 * 验收判据(文档 09 §6 第 2、3 步):
 *   - 板上能输出合理的检测结果(人站在画面里 -> 检出 1 个人,位置正确)
 *   - 打印推理耗时,和 Espressif 给的参考值(约 60ms)对比
 */

#include <inttypes.h>
#include <stdio.h>

#include "cam.h"
#include "disp.h"
#include "infer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "percept";

#define STATS_WINDOW_US   (1000 * 1000)   /* 每秒统计一次 */
#define FRAME_WAIT_MS     1000

#define TARGET_SIZE_WIDTH 480
#define TARGET_SIZE_HEIGHT 480
#define SRC_SIZE_HEIGHT 800
#define SRC_SIZE_WIDTH 800


void app_main(void)
{
    ESP_LOGI(TAG, "感知端启动:采集 + 推理闭环");
    ESP_LOGI(TAG, "PSRAM 可用 %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    ESP_ERROR_CHECK(disp_init());
    ESP_ERROR_CHECK(cam_init());
    ESP_ERROR_CHECK(infer_init());

    const cam_info_t *info = cam_get_info();
    ESP_LOGI(TAG, "采集参数: %" PRIu32 "x%" PRIu32 "  fourcc=0x%08" PRIx32,
             info->width, info->height, info->pixel_format);

    uint32_t window_frames = 0;
    uint64_t total_frames = 0;
    int64_t  window_start = esp_timer_get_time();

    while (true) {
        void *data = NULL;
        size_t len = 0;

        if (cam_acquire_frame(&data, &len, FRAME_WAIT_MS) != ESP_OK) {
            ESP_LOGW(TAG, "取帧失败");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        




        /* ==================== 你的任务:在这里接上推理 ====================
         *
         * 1) 准备结果数组:
         *      infer_box_t boxes[INFER_MAX_BOXES];
         *      size_t count = 0;
         *
         * 2) 调用推理(宽高必须传真实值,后处理靠它把框映射回原图):
         *      infer_run((const uint8_t *)data, info->width, info->height,
         *                boxes, INFER_MAX_BOXES, &count);
         *
         * 3) 打印结果,打印成一行,方便和每秒统计对齐看:
         *      检测: %u 个目标 | [x=.. y=.. w=.. h=.. conf=..]
         *
         * 4) 顺便想一个问题(先想,不用改代码):
         *    推理要 60ms 左右,而采集是 30fps(33ms 一帧)。
         *    如果在取帧循环里同步等推理,取帧帧率会被拖到多少?
         *    这对"采集任务"和"推理任务"该怎么分工有什么启示?
         *    (提示:文档 09 §2.2 的线程与队列方案就是为这个问题设计的)
         */

        infer_box_t boxes[INFER_MAX_BOXES];
        size_t count = 0;

        infer_run((const uint8_t *)data, info->width, info->height, boxes, INFER_MAX_BOXES, &count);
        if(count == 0)
            ESP_LOGI(TAG, "未检测出目标!");
        else
        {
            for(size_t i = 0; i < count; i++)
            {
                ESP_LOGI(TAG, "目标%zu: x = %d  y = %d  w = %d  h = %d  conf = %u%%",
                                    i, boxes[i].x, boxes[i].y, boxes[i].w, boxes[i].h, boxes[i].score_pct);
            }
        }

        uint16_t* src_buf = (uint16_t*)data;
        uint16_t* det_buf = disp_get_framebuffer();

        for(size_t i = 0; i < TARGET_SIZE_HEIGHT; i++)
        {   
            size_t y = i * SRC_SIZE_HEIGHT / TARGET_SIZE_HEIGHT;
            for(size_t j = 0; j < TARGET_SIZE_WIDTH; j++)
            {
            size_t x = j * SRC_SIZE_WIDTH / TARGET_SIZE_WIDTH;

                det_buf[j + i * TARGET_SIZE_WIDTH] = src_buf[x + y * SRC_SIZE_WIDTH];
            }
        }

        /* Draw detection boxes: 800x800 model coords -> 480x480 framebuffer (0.6). */
        const uint16_t box_color = 0xF800; /* RGB565 red */
        for (size_t i = 0; i < count; i++) {
            /* Map all four bounds once, then clamp so every write stays in bounds. */
            int32_t x0 = (int32_t)boxes[i].x * TARGET_SIZE_WIDTH / SRC_SIZE_WIDTH;
            int32_t y0 = (int32_t)boxes[i].y * TARGET_SIZE_HEIGHT / SRC_SIZE_HEIGHT;
            int32_t x1 = ((int32_t)boxes[i].x + boxes[i].w) * TARGET_SIZE_WIDTH / SRC_SIZE_WIDTH;
            int32_t y1 = ((int32_t)boxes[i].y + boxes[i].h) * TARGET_SIZE_HEIGHT / SRC_SIZE_HEIGHT;

            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > TARGET_SIZE_WIDTH - 1)  x1 = TARGET_SIZE_WIDTH - 1;
            if (y1 > TARGET_SIZE_HEIGHT - 1) y1 = TARGET_SIZE_HEIGHT - 1;
            if (x0 > x1 || y0 > y1) {
                continue;
            }

            /* Top and bottom edges: one horizontal run each. */
            uint16_t *top = det_buf + (size_t)y0 * TARGET_SIZE_WIDTH;
            uint16_t *bottom = det_buf + (size_t)y1 * TARGET_SIZE_WIDTH;
            for (int32_t x = x0; x <= x1; x++) {
                top[x] = box_color;
                bottom[x] = box_color;
            }

            /* Left and right edges: one vertical run each. */
            for (int32_t y = y0; y <= y1; y++) {
                uint16_t *row = det_buf + (size_t)y * TARGET_SIZE_WIDTH;
                row[x0] = box_color;
                row[x1] = box_color;
            }
        }
        disp_flush();

        cam_release_frame();
        window_frames++;
        total_frames++;

        const int64_t now = esp_timer_get_time();
        if (now - window_start >= STATS_WINDOW_US) {
            const float seconds = (float)(now - window_start) / 1e6f;
            const infer_stats_t *st = infer_get_stats();
            ESP_LOGI(TAG,
                     "%5.1f fps | 本秒取帧 %" PRIu32 " | 累计 %" PRIu64
                     " | 推理 %" PRIu32 " 次(最近 %" PRIu32 "us 最小 %" PRIu32
                     "us 最大 %" PRIu32 "us) | PSRAM 空闲 %u KB",
                     (float)window_frames / seconds, window_frames, total_frames,
                     st->frames, st->last_us, st->min_us, st->max_us,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            window_frames = 0;
            window_start = now;
        }
    }
}
