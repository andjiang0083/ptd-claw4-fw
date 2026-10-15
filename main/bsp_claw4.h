/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 andjiang0083
 *
 * bsp_claw4.h — Metalio Claw4 板级支持（显示 / 触摸 / 音频 / NVS / 背光）
 *
 * 板级参数全部来自 MetalioClaw4 固件（CloudZao/MetalioClaw4），显示初始化
 * 直接复用它自带的 esp_lcd_nv3051f.c 驱动（含厂商 init command 序列）。
 *
 * 板级事实：
 *   主控        ESP32-P4（双核 RISC-V 480MHz）+ ESP32-C5 协处理器
 *   屏          3.95" MIPI-DSI 720x720，RGB565，2 data lanes
 *   面板驱动    NV3051F（默认）/ FL7707N（宏切换）
 *   DSI PHY LDO  通道 3，2500 mV
 *   触摸        GT911，I2C SDA=GPIO7 SCL=GPIO8，INT=GPIO33（低有效）
 *   背光        LEDC PWM GPIO52
 *   音频        I2S，SPK BCLK=GPIO12 DOUT=GPIO9；MIC WS=GPIO10 DIN=GPIO11
 *
 * ⚠️ 与上游 Tab5 的最大差别：
 *   Tab5 是 Arduino + M5Unified，整机 BSP 免费拿。这里是裸 ESP-IDF，
 *   DSI 时序、GT911、I2S 全部自己配 —— 这是独立固件的真正成本所在。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"

// ════════════════════════════════════════════════════════════════════
// 板级常量（来自 MetalioClaw4 main/boards/metalio-claw-4/config.h）
// ════════════════════════════════════════════════════════════════════
#define CLAW4_LCD_H_RES      720
#define CLAW4_LCD_V_RES      720
#define CLAW4_LCD_BPP        16          // RGB565，与 GfxFb 一致
#define CLAW4_LCD_RST_PIN    3           // 与摄像头共用复位线
#define CLAW4_DSI_LANES      2
#define CLAW4_DSI_LDO_CHAN   3
#define CLAW4_DSI_LDO_MV     2500

#define CLAW4_I2C_PORT       0
#define CLAW4_I2C_SDA        ((gpio_num_t)7)
#define CLAW4_I2C_SCL        ((gpio_num_t)8)
#define CLAW4_TOUCH_INT      ((gpio_num_t)33)   // GT911 INT，低有效

#define CLAW4_BL_PWM_PIN     ((gpio_num_t)52)

// 音频（GPIO 号来自 MetalioClaw4 README；codec 走 I2S 直写）
#include "driver/i2s_common.h"
#define CLAW4_I2S_PORT       ((i2s_port_t)1)
#define CLAW4_I2S_BCLK       ((gpio_num_t)12)
#define CLAW4_I2S_DOUT       ((gpio_num_t)9)
#define CLAW4_I2S_MIC_WS     ((gpio_num_t)10)
#define CLAW4_I2S_MIC_DIN    ((gpio_num_t)11)
#define CLAW4_SPK_SR         16000       // SFX_SR 也是 16kHz，天然对齐

// ════════════════════════════════════════════════════════════════════
// Gfx 实现（PSRAM 帧缓冲 + DSI video mode）
// ════════════════════════════════════════════════════════════════════
// 架构与 Tab5 一致：帧缓冲常驻 PSRAM，由 DSI DMA 连续扫描输出。
// 绘制端（src/gfx_fb.h 的 GfxFb）只管往 fb 里写，不需要知道 DSI 存在。
//
// ⚠️ DSI 集成三条要点：
//   1. fb 必须 heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)——
//      DSI DMA 要读它，缺 DMA capability 会跑飞或花屏。
//   2. DPI 时序与 DSI 速率必须匹配 —— 本 BSP 用厂商宏
//      NV3051F_720_720_PANEL_60HZ_DPI_CONFIG + NV3051F_PANEL_BUS_DSI_2CH_CONFIG，
//      是官方固件实测能亮屏的参数，不要自行"改大试试"。
//   3. video mode 下帧缓冲常驻扫描，无需逐帧提交；commit() 留作
//      "真机出现撕裂时的手动刷新"挂载点。
class GfxClaw4 {
 public:
  // DSI 目标缓冲：RGB888（面板 colmod 0x77 / DSI 流 out_color_format 均为 RGB888）
  uint8_t*  fb888 = nullptr;
  // GfxFb 写入的中间缓冲：RGB565（游戏侧仍按 565 生成）
  uint16_t* fb = nullptr;
  void init(void);
  void setRotation(int) {}             // 方形屏 native 朝向即最终朝向
  void commit(void);
  void setBrightness(int pct);         // 0..100
  int  w() const { return CLAW4_LCD_H_RES; }
  int  h() const { return CLAW4_LCD_V_RES; }
 private:
  void* panel_ = nullptr;
  void* bus_   = nullptr;
};

// ════════════════════════════════════════════════════════════════════
// C 接口（实现文件是 C++，但 BSP 与游戏层之间用 C 链接）
// ════════════════════════════════════════════════════════════════════
#ifdef __cplusplus
extern "C" {
#endif

// ── 触摸 ────────────────────────────────────────────────────────────
struct Claw4TouchPoint { int x, y; bool down; };
bool  touchInit(void);   // 失败返回 false（触摸不可用不阻塞启动）
bool  touchRead(Claw4TouchPoint* out);   // false = 无触摸
int   touchWidth(void);
int   touchHeight(void);

// ── 音频 ────────────────────────────────────────────────────────────
void  audioInit(void);
void  audioPlayRaw(const int16_t* data, size_t len, uint32_t sr);
void  audioSetVolume(int pct);

// ── NVS ─────────────────────────────────────────────────────────────
void     nvsInit(void);
void     nvsSave(void);
uint32_t nvsGetU32(const char* key, uint32_t def);
uint16_t nvsGetU16(const char* key, uint16_t def);
uint8_t  nvsGetU8(const char* key, uint8_t def);
void     nvsPutU32(const char* key, uint32_t v);
void     nvsPutU16(const char* key, uint16_t v);
void     nvsPutU8(const char* key, uint8_t v);

// ── 杂项 ────────────────────────────────────────────────────────────
// 游戏层（src/game.h）以 uint32_t millis(void) 使用时间戳 —— Arduino 的
// millis() 本来就是 uint32_t 回绕语义，改成 uint64_t 会让所有
// (uint32_t)(millis() + delay) 表达式隐式截断。此处保持一致。
uint32_t millis(void);
void     delayMs(uint32_t ms);
void     feedWdt(void);

#ifdef __cplusplus
}
#endif

#endif  // SOC_MIPI_DSI_SUPPORTED
