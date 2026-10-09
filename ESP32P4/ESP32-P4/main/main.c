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

#include "esp_painter.h"
#include "esp_painter_font.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "percept";

/*任务共享*/
static cam_frame_t       s_slot;        /*槽位里的帧*/
static bool              s_slot_valid;  /*槽位里是否有有效帧*/
static SemaphoreHandle_t s_slot_mutex;  /*只保护上面两个变量*/
static SemaphoreHandle_t s_frame_ready; /*槽位从空变为非空时候give*/
static uint32_t          s_dropped;     /*被覆盖丢弃的帧数*/

#define STATS_WINDOW_US   (1000 * 1000)   /* 每秒统计一次 */
#define FRAME_WAIT_MS     1000

#define TARGET_SIZE_WIDTH 480
#define TARGET_SIZE_HEIGHT 480
#define SRC_SIZE_HEIGHT 800
#define SRC_SIZE_WIDTH 800

#define TRACK_MAX 8   /*对齐INFER_MAX_BOXES*/
#define TRACK_IOU_THR  0.55f
#define TRACK_MIN_HITS 3 /*连续两帧确认才输出*/
#define TRACK_MAX_LOST 25 /*30fsp下最多丢失0.125s*/

typedef struct 
{
    uint8_t used;
    uint8_t hits;  /*连续命中帧数*/
    uint8_t lost;   /*连续丢失帧数*/
    uint16_t id;    /*0 = 无效 （0保留位‘无’）*/
    infer_box_t box;    /*最近一次的框 */
}track_t;

static track_t s_tracks[TRACK_MAX];
static uint16_t s_next_id = 1;

esp_painter_handle_t painter = NULL;
const esp_painter_config_t config = {
    .canvas = {
        .width  = TARGET_SIZE_WIDTH,
        .height = TARGET_SIZE_HEIGHT,
    },
    .color_format = ESP_PAINTER_COLOR_FORMAT_RGB565,
    .default_font = &esp_painter_basic_font_24,  // requires CONFIG_ESP_PAINTER_BASIC_FONT_24=y
    .flags = {
        .swap_color_bytes = 0,
    },
};

static const esp_painter_box_style_t box_style = {
    .line_width  = 2,
    .box_color   = ESP_PAINTER_COLOR_GREEN,
    .font        = NULL,                       
    .text_color  = ESP_PAINTER_COLOR_GREEN,
};


static float box_iou(const infer_box_t *a, const infer_box_t *b)
{
    const int ax2 = a->x + a->w;
    const int ay2 = a->y + a->h;
    const int bx2 = b->x + b->w;
    const int by2 = b->y + b->h;

    const int ix1 = (a->x > b->x) ? a->x : b->x;
    const int iy1 = (a->y > b->y) ? a->y : b->y;
    const int ix2 = (ax2 < bx2) ? ax2 : bx2;
    const int iy2 = (ay2 < by2) ? ay2 : by2;

    const int iw = ix2 - ix1;
    const int ih = iy2 - iy1;
    if (iw <= 0 || ih <= 0) return 0.0f;

    const float inter = (float)iw * (float)ih;
    const float uni   = (float)a->w * a->h + (float)b->w * b->h - inter;
    return (uni > 0.0f) ? (inter / uni) : 0.0f;
}


/*返回 -1.0（最左） ~ +1.0（最右）， 0 = 正中*/
static float box_bearing_norm(const infer_box_t *b, uint32_t frame_w)
{
    const float cx = (float)b->x + (float)b->w * 0.5f;
    return (cx - (float)frame_w * 0.5f) / ((float)frame_w * 0.5f);
}


/**
 * 用本帧检测结果更新轨迹，并把"已确认"的轨迹输出到out[]
 * 
 * @param dets 本帧检测框
 * @param n 框数
 * @param out [out]已确认的轨迹
 * @param max_out out容量
 * @return 实际输出的轨迹数
 */
static size_t track_update(const infer_box_t *dets, size_t n, track_t *out, size_t max_out)
{
    bool matched_det[INFER_MAX_BOXES] = {false};
    bool matched_trk[TRACK_MAX] = {false};

    /*为每一个存在的框寻找最佳匹配框*/
    for(size_t t = 0; t < TRACK_MAX; t++)
    {
        if(!s_tracks[t].used) continue;

        int best = -1;
        float best_iou = TRACK_IOU_THR; /*低于阈值不匹配*/
        
        for(size_t d = 0; d < n; d++)
        {
            if(matched_det[d]) continue;
            const float v = box_iou(&s_tracks[t].box, &dets[d]);
            if(v > best_iou)
            {
                best_iou = v;
                best = (int)d;
            }
        }
        if(best >= 0)
        {
            matched_det[best] = true;
            matched_trk[t] = true;
            s_tracks[t].box = dets[best];
            s_tracks[t].lost = 0;
            if(s_tracks[t].hits < 255)
                s_tracks[t].hits++;
        }
    }

    /*未匹配的框*/
    for(size_t d = 0; d < n; d++)
    {
        if(matched_det[d]) continue;

        for(size_t t = 0; t < TRACK_MAX; t++)
        {
            if(s_tracks[t].used) continue;

            s_tracks[t].used = 1;
            s_tracks[t].hits = 1;
            s_tracks[t].lost = 0;
            s_tracks[t].box = dets[d];
            s_tracks[t].id = s_next_id++;


            /*ID单调递增 回绕跳过0*/
            if(!s_next_id) s_next_id = 1;

            matched_trk[t] = true; /*新建帧不算丢失*/
            break;
        }
    }

    /*未匹配的轨迹丢失*/
    for(size_t t= 0; t < TRACK_MAX; t++)
    {
        if(!s_tracks[t].used) continue;
        if(matched_trk[t]) continue;

        s_tracks[t].lost++;
        if(s_tracks[t].lost > TRACK_MAX_LOST)
        {
            ESP_LOGI(TAG, "轨迹 : %u 消失(暂失 %u 帧)", s_tracks[t].id, s_tracks[t].lost);
            s_tracks[t].used = 0;
        }
    }

    /*只输出已经确认并且当前可见的轨迹*/
    size_t cnt = 0;
    for(size_t t = 0; t < TRACK_MAX && cnt < max_out; t++)
    {
        if(!s_tracks[t].used) continue;
        if(s_tracks[t].lost != 0) continue;   /*暂时不输出*/
        if(s_tracks[t].hits < TRACK_MIN_HITS) continue;  /*min_hits过滤*/
        out[cnt++] = s_tracks[t];
    }
    return cnt;

}


/*绘制推理结果框*/
static void draw_box(uint16_t *fb, int cx, int cy, int cw, int ch)
{
    double proportion_x = (double)TARGET_SIZE_WIDTH / SRC_SIZE_WIDTH;
    double proportion_y = (double)TARGET_SIZE_HEIGHT / SRC_SIZE_HEIGHT;
    
    /*(X0,Y0):左上, (x1,y1)右下*/
    int x = cx * proportion_x;
    int y = cy * proportion_y;
    int w = cw * proportion_x;
    int h = ch * proportion_y;

    size_t fb_size = TARGET_SIZE_WIDTH * TARGET_SIZE_HEIGHT * 2;
    esp_painter_draw_detection_box(painter, (uint8_t *)fb, fb_size,
                               x, y, w, h,
                               &box_style, NULL);
}

static void cam_task(void* arg)
{
    uint32_t window_frames = 0;
    int64_t window_start = esp_timer_get_time();
        cam_frame_t f;

    for(;;)
    {
        if(cam_acquire_frame(&f, FRAME_WAIT_MS) != ESP_OK)
        {
            ESP_LOGW(TAG, "取帧失败!");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        cam_frame_t old;   /*旧帧存放*/
        bool had_old = false;


        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);

        if(s_slot_valid == true)
        {
            old = s_slot;
            had_old = true;   
        }

        s_slot = f;
        s_slot_valid = true;
        xSemaphoreGive(s_slot_mutex);

        xSemaphoreGive(s_frame_ready);

        if(had_old)
        {
            cam_release_frame(&old);
            s_dropped++;
        }

        window_frames++;

        const int64_t now = esp_timer_get_time();
            if (now - window_start >= STATS_WINDOW_US) {
                const float seconds = (float)(now - window_start) / 1e6f;
                ESP_LOGI(TAG, "[采集] %5.1f fps | 本秒 %u | 丢弃累计 %u | PSRAM 空闲 %u KB",
                            (float)window_frames / seconds, window_frames, s_dropped,
                            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
                window_frames = 0;
                window_start = now;
        }
    }
    

}   

void infer_task(void* arg)
{

    const cam_info_t *info = cam_get_info();
    uint32_t window_frames = 0;
    int64_t window_start  = esp_timer_get_time();
    
    for(;;)
    {
        vTaskDelay(1);  /*给空闲喂狗*/

        xSemaphoreTake(s_frame_ready, portMAX_DELAY);
        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        cam_frame_t f;
        if(!s_slot_valid)
        {
            xSemaphoreGive(s_slot_mutex);
            continue;
        }
        else
        {
            f = s_slot;
            s_slot_valid = false;
        }

        xSemaphoreGive(s_slot_mutex);
        infer_box_t boxes[INFER_MAX_BOXES];
        size_t count = 0;

        if(infer_run((const uint8_t *)f.data, info->width, info->height, boxes, INFER_MAX_BOXES, &count) != ESP_OK)
        {
            cam_release_frame(&f);
            continue;
        }

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

        uint16_t* src_buf = (uint16_t*)f.data;
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


        static track_t tracks_out[TRACK_MAX];
        const size_t tn = track_update(boxes, count, tracks_out, TRACK_MAX);

        for(size_t i = 0; i < tn; i++)
        {
            ESP_LOGI(TAG, "ID %2u: x = %d  y = %d  w = %d  h = %d  conf = %u%%  hits = %u",
                     tracks_out[i].id, tracks_out[i].box.x, tracks_out[i].box.y,
                     tracks_out[i].box.w, tracks_out[i].box.h,
                     tracks_out[i].box.score_pct, tracks_out[i].hits);
        }

        for(size_t i = 0; i < count; i++)
        {
            draw_box(det_buf, 
                tracks_out[i].box.x, 
                tracks_out[i].box.y, 
                tracks_out[i].box.w, 
                tracks_out[i].box.h);
            }
        disp_flush();

        cam_release_frame(&f);

        /* static 计数器,在 track_update 里累加 */
        static uint32_t s_trk_new, s_trk_del;
        
        window_frames++;
        const int64_t now = esp_timer_get_time();
        if (now - window_start >= STATS_WINDOW_US) {
            const float seconds = (float)(now - window_start) / 1e6f;
            const infer_stats_t *st = infer_get_stats();
            ESP_LOGI(TAG, "[推理] %5.1f fps | 最近 %u us 最小 %u us 最大 %u us",
                     (float)window_frames / seconds, st->last_us, st->min_us, st->max_us);
            ESP_LOGI(TAG, "[跟踪] 当前 %zu 条 | 本秒新建 %u 删除 %u", tn, s_trk_new, s_trk_del);
            s_trk_new = s_trk_del = 0;
            window_frames = 0;
            window_start = now;
        }

    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(disp_init());
    ESP_ERROR_CHECK(cam_init());
    ESP_ERROR_CHECK(infer_init());
    ESP_ERROR_CHECK(esp_painter_init(&config, &painter));

    s_slot_mutex = xSemaphoreCreateMutex();
    s_frame_ready = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(cam_task, "cam", 4096, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(infer_task, "infer", 8192, NULL, 5, NULL, 1);


}
