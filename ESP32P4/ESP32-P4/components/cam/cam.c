/*
 * cam 组件实现 —— 见 include/cam.h 的说明
 *
 * 板级参数来源:厂商公开工程 esp32-p4c5-module-dev-board / camera-mipi-csi-sc2336
 * 的 Kconfig 默认值,并已在本板实测验证(见 doc/rules/10 §6.1):
 *   SCCB SCL=GPIO8  SDA=GPIO7
 *   SC2336 RESET=GPIO26,PWDN 未接
 */

#include "cam.h"

#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

static const char *TAG = "cam";

/* ---- 板级引脚(已确认) ---- */
#define CAM_SCCB_SCL_GPIO    8
#define CAM_SCCB_SDA_GPIO    7
#define CAM_SC2336_RST_GPIO  26
#define CAM_SCCB_I2C_PORT    0
#define CAM_SCCB_FREQ_HZ     100000

/* 环形缓冲个数。2 个即可让"取帧—处理"与"采集"重叠,
 * 具体数量在文档 09 §2.3 的队列设计里可再调。 */
#define CAM_BUFFER_COUNT     4

typedef struct {
    int fd;
    cam_info_t info;
    cam_stats_t stats;
    uint8_t *buffers[CAM_BUFFER_COUNT];
    size_t   lengths[CAM_BUFFER_COUNT];
} cam_ctx_t;

static cam_ctx_t s_cam;

/* 帧缓冲必须按 cache 行对齐,否则 esp_cache_msync 无法安全使用 */
static size_t cam_buffer_alignment(void)
{
    size_t alignment = 0;
    if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alignment) != ESP_OK || alignment == 0) {
        alignment = 64;
    }
    return alignment;
}

static esp_err_t cam_init_video_hw(void)
{
    /* 板内 I2C 还有触摸与音频,这里由 esp_video 自建 SCCB 总线;
     * 将来显示子系统接手后再改成"共享同一条总线"。 */
    esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port = CAM_SCCB_I2C_PORT,
                .scl_pin = CAM_SCCB_SCL_GPIO,
                .sda_pin = CAM_SCCB_SDA_GPIO,
            },
            .freq = CAM_SCCB_FREQ_HZ,
        },
        .reset_pin = CAM_SC2336_RST_GPIO,
        .pwdn_pin = -1,          /* 板上未使用 PWDN */
        .dont_init_ldo = false,  /* 让 esp_video 初始化 MIPI PHY 的 LDO */
    };
    const esp_video_init_config_t video_config = {
        .csi = &csi_config,
    };
    return esp_video_init(&video_config);
}

static esp_err_t cam_open_and_configure(void)
{
    cam_ctx_t *cam = &s_cam;
    const char *dev_path = ESP_VIDEO_MIPI_CSI_DEVICE_NAME;

    cam->fd = open(dev_path, O_RDONLY);
    if (cam->fd < 0) {
        ESP_LOGE(TAG, "打开 %s 失败", dev_path);
        return ESP_FAIL;
    }

    struct v4l2_format format = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(cam->fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "读取当前格式失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "设备默认格式: %lux%lu fourcc=0x%08" PRIx32,
             (unsigned long)format.fmt.pix.width, (unsigned long)format.fmt.pix.height,
             (uint32_t)format.fmt.pix.pixelformat);

    cam->info.width = format.fmt.pix.width;
    cam->info.height = format.fmt.pix.height;
    cam->info.pixel_format = format.fmt.pix.pixelformat;
    cam->info.buffer_count = CAM_BUFFER_COUNT;

    struct v4l2_requestbuffers req = {
        .count = CAM_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_USERPTR,
    };
    if (ioctl(cam->fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "申请缓冲失败");
        return ESP_FAIL;
    }

    for (int i = 0; i < CAM_BUFFER_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_USERPTR,
            .index = i,
        };
        if (ioctl(cam->fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "查询缓冲 %d 失败", i);
            return ESP_FAIL;
        }

        cam->lengths[i] = buf.length;
        cam->buffers[i] = heap_caps_aligned_calloc(cam_buffer_alignment(), 1, buf.length,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (cam->buffers[i] == NULL) {
            ESP_LOGE(TAG, "缓冲 %d 分配失败(需要 %u 字节 PSRAM)", i, (unsigned)buf.length);
            return ESP_ERR_NO_MEM;
        }

        buf.m.userptr = (unsigned long)cam->buffers[i];
        buf.length = cam->lengths[i];
        if (ioctl(cam->fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "入队缓冲 %d 失败", i);
            return ESP_FAIL;
        }
    }
    cam->info.buffer_size = cam->lengths[0];

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam->fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "启动出流失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "出流已启动: %" PRIu32 "x%" PRIu32 ",%d 个缓冲,每个 %u KB",
             cam->info.width, cam->info.height, CAM_BUFFER_COUNT,
             (unsigned)(cam->info.buffer_size / 1024));
    return ESP_OK;
}

esp_err_t cam_init(void)
{
    cam_ctx_t *cam = &s_cam;
    memset(cam, 0, sizeof(*cam));
    cam->fd = -1;

    ESP_RETURN_ON_ERROR(cam_init_video_hw(), TAG, "摄像头硬件初始化失败");
    ESP_RETURN_ON_ERROR(cam_open_and_configure(), TAG, "摄像头打开/配置失败");

    ESP_LOGI(TAG, "摄像头就绪");
    return ESP_OK;
}

const cam_info_t *cam_get_info(void)
{
    return &s_cam.info;
}

void cam_get_stats(cam_stats_t *out)
{
    if (out) {
        *out = s_cam.stats;
    }
}

esp_err_t cam_acquire_frame(cam_frame_t *out, uint32_t timeout_ms)
{
    cam_ctx_t *cam = &s_cam;
    (void)timeout_ms;   /* 超时语义尚未实现,见 cam.h 的说明 */
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "参数为空");
    ESP_RETURN_ON_FALSE(cam->fd >= 0, ESP_ERR_INVALID_STATE, TAG, "摄像头未初始化");

    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_USERPTR,
    };

    if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) != 0) {
        cam->stats.failures++;
        ESP_LOGE(TAG, "DQBUF 失败");
        return ESP_FAIL;
    }
    if (buf.bytesused == 0) {
        cam->stats.failures++;
        ioctl(cam->fd, VIDIOC_QBUF, &buf);
        return ESP_FAIL;
    }

    /* DMA 写的是 PSRAM,CPU 读之前必须把 cache 刷成最新 */
    const size_t alignment = cam_buffer_alignment();
    const size_t sync_size = (buf.bytesused + alignment - 1) / alignment * alignment;
    esp_err_t err = esp_cache_msync(cam->buffers[buf.index], sync_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cache 同步失败: %s", esp_err_to_name(err));
    }

    /* 把"这一帧是谁"完整交给调用方 —— 归还时凭 index 认领,不再依赖内部状态。
     * 这样就能同时持有两帧:一边拿着新帧,一边把旧帧还回去。 */
    out->data  = cam->buffers[buf.index];
    out->len   = buf.bytesused;
    out->index = buf.index;

    cam->stats.frames++;
    cam->stats.last_frame_bytes = buf.bytesused;
    return ESP_OK;
}

esp_err_t cam_release_frame(const cam_frame_t *frame)
{
    cam_ctx_t *cam = &s_cam;

    ESP_RETURN_ON_FALSE(frame != NULL, ESP_ERR_INVALID_ARG, TAG, "参数为空");
    /* 缓冲号来自内部,但仍然检查一次 —— 防御性检查很便宜,越界写的代价极高 */
    ESP_RETURN_ON_FALSE(frame->index >= 0 && frame->index < CAM_BUFFER_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "缓冲号越界: %d", frame->index);

    const int index = frame->index;
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_USERPTR,
        .index = index,
        .m.userptr = (unsigned long)cam->buffers[index],
        .length = cam->lengths[index],
    };
    if (ioctl(cam->fd, VIDIOC_QBUF, &buf) != 0) {
        cam->stats.failures++;
        ESP_LOGE(TAG, "归还缓冲 %d 失败", index);
        return ESP_FAIL;
    }

    return ESP_OK;
}
