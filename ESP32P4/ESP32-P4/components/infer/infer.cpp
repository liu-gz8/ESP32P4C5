/*
 * infer 组件实现 —— 用 esp-dl 的行人检测模型(pedestrian_detect_pico_s8_v1)
 *
 * 模型信息(来自组件 README,已在 IDF 6.1 上编译验证):
 *   输入 224x224x3,int8 量化,模型文件 435KB,存放方式 = flash RODATA
 *   Espressif 给出的 ESP32-P4 耗时参考:前处理 3.5ms + 模型 55.6ms + 后处理 1.4ms
 *
 * 重要:esp-dl 的后处理内部已经把框映射回"原图坐标系"(用 inv_resize_scale),
 *       所以我们不做坐标变换,但必须把真实的 width/height 传给 img_t。
 */

#include "infer.h"

#include <list>

#include "esp_log.h"
#include "esp_timer.h"

#include "dl_image_define.hpp"     /* C++:img_t / pix_type_t */
#include "pedestrian_detect.hpp"   /* C++:PedestrianDetect */

#include "dl_detect_base.hpp"

static const char *TAG = "infer";

static PedestrianDetect *s_detect = nullptr;
static infer_stats_t s_stats;

extern "C" esp_err_t infer_init(void)
{
    if (s_detect != nullptr) {
        return ESP_OK;              /* 幂等:重复调用不重复加载 */
    }

    s_detect = new PedestrianDetect();
    if (s_detect == nullptr) {
        ESP_LOGE(TAG, "模型对象创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "行人检测模型已加载(输入 224x224x3)");
    return ESP_OK;
}

extern "C" esp_err_t infer_run(const uint8_t *rgb565, uint32_t width, uint32_t height,
                               infer_box_t *boxes, size_t max_boxes, size_t *out_count)
{
    if (s_detect == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rgb565 == nullptr || boxes == nullptr || out_count == nullptr || max_boxes == 0) {
        return ESP_ERR_INVALID_ARG;
    }


    /* ==================== TODO 1:把这一帧包装成 esp-dl 的图像结构 ====================
     * 需要用到的字段(去看 dl_image_define.hpp 里 img_s 的定义):
     *     img.data     = 帧数据指针
     *     img.width    = ?          ← 用函数参数,不要写死数字
     *     img.height   = ?
     *     img.pix_type = ?          ← 两个候选:DL_IMAGE_PIX_TYPE_RGB565LE / ...BE
     *
     * 判断方法(先推、再实测):
     *   - 我们相机的 fourcc 是 0x50424752,拆开是 'R','G','B','P'
     *   - V4L2 的 fourcc 是按 a | b<<8 | c<<16 | d<<24 拼的
     *   - 想清楚这个字节序对应 esp-dl 的 LE 还是 BE,填进去
     *   - 如果填错,现象是"检测结果完全没有意义"(框乱飘/置信度极低),
     *     所以先写一版,后面用真实画面验证
     */
    dl::image::img_t img = {
                            .data = (void*)rgb565,
                            .width = (uint16_t)width,
                            .height = (uint16_t)height,
                            .pix_type = dl::image::pix_type_t::DL_IMAGE_PIX_TYPE_BGR565LE,
                            };


    /* ==================== TODO 2:调用推理并计时 ====================
     *   int64_t t_begin = esp_timer_get_time();
     *   std::list<dl::detect::result_t> &results = s_detect->run(img);
     *   uint32_t cost_us = (uint32_t)(esp_timer_get_time() - t_begin);
     *
     * 注意:run() 内部一次做完了 前处理 + 模型 + 后处理,接口上无法分开计时,
     *       所以我们只能记录总耗时——这个限制要如实反映在日志里,不要假装分段测了。
     */

     int64_t t_begin = esp_timer_get_time();
     std::list<dl::detect::result_t> &results = s_detect->run(img);
     uint32_t cost_us = (uint32_t)(esp_timer_get_time() - t_begin);
     //ESP_LOGI(TAG, "run() 单次耗时: %u", cost_us);


    /* ==================== TODO 3:把结果用拷进调方的数组 ====================
     * 规则(照着做,否则容易写出越界或坐标错的 bug):
     *
     *   1. 最多写 max_boxes 个;超出部分丢弃(后面要统计丢弃数)
     *
     *   2. result_t.box 的格式是 [left, top, right, bottom],且 right/bottom 是"包含"的
     *        → x = box[0]
     *          y = box[1]
     *          w = box[2] - box[0] + 1      ← 想清楚这个 +1 为什么存在
     *          h = box[3] - box[1] + 1
     *
     *   3. score 是 0.0~1.0 的浮点,转成 0~100 的整数存进 score_pct(方便打印)
     *      category 直接拷
     *
     *   4. 更新统计:s_stats.frames++、last_us、min_us、max_us
     *      (注意 min_us 的初值:如果你写 0,那最小值就永远是 0 了——想个办法)
     */
    size_t n = 0;

    for(auto &det : results)
    {
        if(n >= max_boxes)
        {
            break;
        }
        boxes[n].x = det.box[0];
        boxes[n].y = det.box[1];
        boxes[n].w = det.box[2] - det.box[0] + 1;
        boxes[n].h = det.box[3] - det.box[1] + 1;
        boxes[n].score_pct = (uint16_t)(det.score * 100.0f);
        boxes[n].category = (uint8_t)det.category;
        n++;
    }    

    s_stats.frames++;
    s_stats.last_us = cost_us;
    s_stats.min_us = s_stats.min_us == 0 ? s_stats.last_us : (s_stats.min_us < s_stats.last_us ? s_stats.min_us : s_stats.last_us);
    s_stats.max_us = s_stats.max_us > s_stats.last_us ? s_stats.max_us : s_stats.last_us;                    

    

    *out_count = n;
    return ESP_OK;
}

extern "C" const infer_stats_t *infer_get_stats(void)
{
    return &s_stats;
}
