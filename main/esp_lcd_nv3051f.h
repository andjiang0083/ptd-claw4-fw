/* SPDX-License-Identifier: MIT
 *
 * esp_lcd_nv3051f.h — 精简版声明（Claw4 移植用）
 *
 * ⚠️ 为什么不用 MetalioClaw4 里的原始头文件：
 *   原文件里的三个初始化宏（NV3051F_PANEL_BUS_DSI_2CH_CONFIG /
 *   NV3051F_PANEL_IO_DBI_CONFIG / NV3051F_720_720_PANEL_60HZ_DPI_CONFIG）
 *   是按较新的 ESP-IDF 写的，在 IDF 5.5.2 下有两处编译不过：
 *     ① phy_clk_src = 0  裸赋值不能隐式转 mipi_dsi_phy_pllref_clock_source_t
 *     ② .flags.use_dma2d 字段在 5.5 的 dpi_panel_config_t 里不存在
 *   本移植把这三个宏的**等价数值**手写进 bsp_claw4.cpp（1000Mbps / 36MHz /
 *   官方 720x720 时序），值与厂商宏完全一致，因此这里只需要保留**函数声明**。
 *
 *   驱动实现 esp_lcd_nv3051f.c 原样使用（它自带的 init command 序列
 *   是这块屏能亮的关键，不要改动）。
 */
#pragma once

#include <stdint.h>
#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} nv3051f_lcd_init_cmd_t;

typedef struct {
    const nv3051f_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        esp_lcd_dsi_bus_handle_t dsi_bus;
        const esp_lcd_dpi_panel_config_t *dpi_config;
    } mipi_config;
} nv3051f_vendor_config_t;

/**
 * @brief 创建 NV3051F 面板（含厂商 init command 序列）
 */
esp_err_t esp_lcd_new_panel_nv3051f(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief 重新发送 NV3051F 厂商初始化 DCS 序列
 *
 * GPIO3 与摄像头共用复位线，摄像头驱动初始化会拉低复位脚导致 LCD 内部
 * 寄存器丢失。本函数只重发 init command，不碰 DPI 流水线。
 */
esp_err_t esp_lcd_nv3051f_replay_vendor_init(esp_lcd_panel_io_handle_t io);

#ifdef __cplusplus
}
#endif

#endif  // SOC_MIPI_DSI_SUPPORTED
