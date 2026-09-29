/*
 * disp 组件实现 —— ST7102 MIPI-DSI 面板初始化
 *
 * 参考文件(已适配 IDF 6.1,但要剥掉 LVGL 依赖):
 *   probe/osptek-camera-demo/components/hw_init/lcd/lcd_init_mipi_dsi.c
 *
 * 对照它写,但三类内容要分开对待,每个 TODO 都标了:
 *   [照抄] 屏厂/厂商给的值,不能改
 *   [理解] 由 IDF API 决定的写法
 *   [简化] 我们不用 LVGL,要去掉的部分
 */

#include "disp.h"

#include <string.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7102.h"
#include "esp_log.h"

static const char *TAG = "disp";

/* 板级参数(已确认,不要改) */
#define DISP_LDO_CHAN         3
#define DISP_LDO_VOLTAGE_MV   2500
#define DISP_LCD_BPP          16

static esp_lcd_dsi_bus_handle_t  s_dsi_bus;
static esp_lcd_panel_io_handle_t s_dbi_io;
static esp_lcd_panel_handle_t    s_panel;
static uint16_t                 *s_framebuffer;

/* ============================================================================
 * TODO 1 [照抄]:屏厂给的初始化命令表
 *
 * 从参考文件顶部原样复制 lcd_init_cmds[] 整张表。
 * 一个字节都不能改 —— 这是屏厂针对面板调的寄存器序列,自己编必坏
 * (花屏/不亮/闪),而且不会有明确报错可查。
 * 表末两条 0x11 / 0x29 是"退出睡眠"和"开显示",后面的毫秒延时是必须的。
 * ========================================================================== */
static const st7102_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x99, (uint8_t []){0x71,0x02,0xa2}, 3, 0},
    {0x99, (uint8_t []){0x71,0x02,0xa3}, 3, 0},
    {0x99, (uint8_t []){0x71,0x02,0xa4}, 3, 0},
    {0xB0, (uint8_t []){0x22,0x61,0x1E,0x61,0x2F,0x39,0x39}, 7, 0},
    {0xB7, (uint8_t []){0x46,0x46}, 2, 0},
    {0xBF, (uint8_t []){0x50,0x50}, 2, 0},
    {0xD7, (uint8_t []){0x00,0x0C,0x80,0x08,0xF0,0xF0}, 6, 0},
    {0xA3, (uint8_t []){0x40,0x03,0x80,0x00,0x44,0x00,0x00,0x00,0x00,0x04,
                        0x6F,0x6F,0x00,0x1A,0x00,0x45,0x05,0x00,0x00,0x00,
                        0x00,0x46,0x00,0x00,0x12,0x20,0x52,0x00,0x05,0x00,
                        0x00,0xFF}, 32, 0},
    {0xA6, (uint8_t []){0x08,0x00,0x24,0x55,0x35,0x00,0x76,0x40,0x58,0x58,
                        0x00,0x24,0x55,0x00,0x00,0x40,0x40,0x58,0x58,0x02,
                        0xAC,0x51,0x00,0xCC,0x40,0x40,0x58,0x58,0x00,0xAC,
                        0x11,0x00,0x00,0x40,0x40,0x58,0x58,0x00,0x00,0x06,
                        0x00,0x00,0x00,0x00}, 44, 0},
    {0xA7, (uint8_t []){0x19,0x19,0x00,0x64,0x40,0x07,0x16,0x40,0x00,0x44,
                        0x43,0x58,0x58,0x00,0x64,0x40,0x25,0x34,0x00,0x00,
                        0x42,0x41,0x58,0x58,0x00,0x64,0x40,0x4B,0x5A,0x00,
                        0x00,0x42,0x41,0x58,0x58,0x00,0x24,0x40,0x69,0x78,
                        0x00,0x00,0x40,0x40,0x58,0x58,0x00,0x44}, 48, 0},
    {0xAC, (uint8_t []){0x00,0x1C,0x04,0x1A,0x19,0x1B,0x1B,0x18,0x06,0x13,
                        0x19,0x11,0x1B,0x08,0x18,0x0A,0x01,0x1C,0x04,0x1A,
                        0x19,0x01,0x1B,0x18,0x06,0x12,0x19,0x10,0x1B,0x09,
                        0x18,0x0B,0xBF,0xAA,0xBF,0xAA,0x00}, 37, 0},
    {0xAD, (uint8_t []){0xCC,0x40,0x46,0x11,0x04,0x6F,0x6F}, 7, 0},
    {0xE8, (uint8_t []){0x30,0x07,0x05,0x74,0x74,0x9C,0x00,0xE2,0x04,0x00,
                        0x00,0x00,0x00,0xEF}, 14, 0},
    {0x75, (uint8_t []){0x03,0x04}, 2, 0},
    {0xE7, (uint8_t []){0x8B,0x3C,0x00,0x0C,0xF0,0x5D,0x00,0x5D,0x00,0x5D,
                        0x00,0x5D,0x00,0xFF,0x00,0x08,0x7B,0x00,0x00,0xC8,
                        0x6A,0x5A,0x08,0x1A,0x3C,0x00,0xA1,0x01,0x8C,0x01,
                        0x7F,0xF0,0x22}, 33, 0},
    {0xE9, (uint8_t []){0x3C,0x7F,0x08,0x07,0x1A,0x7A,0x22,0x1A,0x33}, 9, 0},
    {0xC8, (uint8_t []){0x00,0x00,0x15,0x26,0x44,0x00,0x78,0x03,0xBE,0x06,
                        0x11,0x1C,0x09,0x8A,0x03,0x21,0xD4,0x01,0x11,0x0F,
                        0x22,0x4A,0x0F,0x8F,0x0A,0x32,0xF0,0x0A,0x41,0x0D,
                        0xF3,0x80,0x0D,0xAE,0xC5,0x03,0xC4}, 37, 0},
    {0xC9, (uint8_t []){0x00,0x00,0x15,0x26,0x44,0x00,0x78,0x03,0xBE,0x06,
                        0x11,0x1C,0x09,0x8A,0x03,0x21,0xD4,0x01,0x11,0x0F,
                        0x22,0x4A,0x0F,0x8F,0x0A,0x32,0xF0,0x0A,0x41,0x0D,
                        0xF3,0x80,0x0D,0xAE,0xC5,0x03,0xC4}, 37, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 20},
};

/* MIPI 差分信号需要 2.5V 供电 */
static esp_err_t ldo_power_on(void)
{
    esp_ldo_channel_handle_t ldo = NULL;
    const esp_ldo_channel_config_t cfg = {
        .chan_id = DISP_LDO_CHAN,
        .voltage_mv = DISP_LDO_VOLTAGE_MV,
    };
    return esp_ldo_acquire_channel(&cfg, &ldo);
}

esp_err_t disp_init(void)
{
    ESP_LOGI(TAG, "初始化 MIPI-DSI 面板 %dx%d", DISP_H_RES, DISP_V_RES);

    /* ---- TODO 2:先给 MIPI PHY 上电 ----
     * ESP_RETURN_ON_ERROR(ldo_power_on(), TAG, "MIPI PHY LDO 上电失败");
     */

    ESP_RETURN_ON_ERROR(ldo_power_on(), TAG, "MIPI PHY LDO 上电失败");

    /* ---- TODO 3 [理解]:创建 DSI 总线 ----
     * 要填四个字段:bus_id / num_data_lanes / phy_clk_src / lane_bit_rate_mbps
     *
     * 关键值从参考文件拿(2 lane / 900 Mbps)。
     *
     * 注意:参考文件里那个便捷宏 ST7102_MIPI_PANEL_BUS_DSI_2CH_CONFIG()
     * 里面的速率是 500 Mbps(给 2.5 寸屏的),不是我们的参数,所以这里
     * 应该手写结构体,不要用那个宏。
     *
     * phy_clk_src 必须用新名字 MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT。
     *
     * esp_err_t ret = esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus);
     */
    esp_lcd_dsi_bus_handle_t s_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 900, 
    };
    
    esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus);


    /* ---- TODO 4 [理解]:创建 DBI(命令)通道 ----
     * esp_lcd_dbi_io_config_t dbi_config = ST7102_MIPI_PANEL_IO_DBI_CONFIG();
     * esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &s_dbi_io);
     */

    esp_lcd_dbi_io_config_t dbi_config = ST7102_MIPI_PANEL_IO_DBI_CONFIG();
    esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &s_dbi_io);

    /* ---- TODO 5 [照抄+理解]:创建 ST7102 面板 ----
     * 要拼三块配置,缺一不可。
     *
     * (a) dpi_config:
     *     .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT
     *     .dpi_clock_freq_mhz = 30                 [照抄]
     *     .virtual_channel = 0
     *     .num_fbs = 1          我们不用 LVGL,单缓冲够
     *     .in_color_format  = LCD_COLOR_FMT_RGB565    IDF 6.1 新名字
     *     .out_color_format = LCD_COLOR_FMT_RGB565    原来叫 pixel_format
     *     .video_timing = {...}  480x480 和四个 porch 全部照抄参考文件
     *     不要写 .flags.use_dma2d(IDF 6.0 起已删除)
     *
     * (b) vendor_config:把初始化表和上面两块绑起来
     *     .init_cmds / .init_cmds_size
     *     .flags.use_mipi_interface = 1
     *     .mipi_config = { .dsi_bus = s_dsi_bus, .dpi_config = &dpi_config }
     *
     * (c) panel_config:
     *     .reset_gpio_num = -1        屏复位脚不在 P4 上
     *     .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB
     *     .bits_per_pixel = DISP_LCD_BPP
     *     .vendor_config  = &vendor_config
     *
     * esp_lcd_new_panel_st7102(s_dbi_io, &panel_config, &s_panel);
     */

    const esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 30,
        .virtual_channel = 0,
        .num_fbs = 1,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .video_timing = {
            .h_size = 480,
            .v_size = 480,
            .hsync_back_porch = 140,
            .hsync_pulse_width = 2,
            .hsync_front_porch = 120,
            .vsync_back_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_front_porch = 200,
        },
        .flags.disable_lp = false,
    };
    st7102_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(st7102_lcd_init_cmd_t),
        .flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_config,

        },
    };

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = DISP_LCD_BPP,
        .vendor_config = &vendor_config,
    };

    esp_lcd_new_panel_st7102(s_dbi_io, &panel_config, &s_panel);

    /* ---- TODO 6 [理解]:初始化顺序不能颠倒 ----
     *   esp_lcd_panel_reset(s_panel);
     *   esp_lcd_panel_init(s_panel);
     *   esp_lcd_dpi_panel_enable_dma2d(s_panel);
     *
     * 验收点:面板初始化会回读 ID,日志里应出现
     *   I (xxxx) st7102: LCD ID: 80 A0 FB
     */

     esp_lcd_panel_reset(s_panel);
     esp_lcd_panel_init(s_panel);
     esp_lcd_dpi_panel_enable_dma2d(s_panel);
    /* ---- TODO 7 [理解]:取帧缓冲 ----
     * esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, (void **)&s_framebuffer);
     * 记得判空。
     *
     * 再打印它的地址 —— 如果 >= 0x48000000 说明在 PSRAM(那我们 flush 时
     * 必须做缓存回写);如果在 0x4FF... 附近就是内部 SRAM。
     */
    if(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, (void**)&s_framebuffer) != ESP_OK)
    {
        ESP_LOGI(TAG, "取帧失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "帧缓冲地址: %p", s_framebuffer);

    
    ESP_LOGI(TAG, "显示初始化完成");
    return ESP_OK;
}

uint16_t *disp_get_framebuffer(void)
{
    return s_framebuffer;
}

esp_err_t disp_flush(void)
{
    /* ---- TODO 8 [理解]:把 CPU 写的像素推给 DMA ----
     *
     * 帧缓冲若在 PSRAM,CPU 写的内容可能还在 cache 里,而 DPI 硬件是直接
     * 从内存读的。所以要回写:
     *
     *   esp_cache_msync(s_framebuffer, DISP_H_RES * DISP_V_RES * 2,
     *                   ESP_CACHE_MSYNC_FLAG_DIR_C2M);
     *
     * 方向 C2M(Cache -> Memory)和相机那边的 M2C 正好相反。
     * 不做这一步的现象:屏上是旧内容 / 大块残影 / 撕裂。
     */
    esp_cache_msync(s_framebuffer, DISP_H_RES * DISP_V_RES * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    return ESP_OK;
}
