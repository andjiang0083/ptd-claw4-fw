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

// ════════════════════════════════════════════════════════════════════
// 游戏帧缓冲的像素格式 = 24（RGB888）
// ════════════════════════════════════════════════════════════════════
// v0.5.1：必须在 include src/gfx_fb.h **之前**定义（main.cpp 里
// bsp_claw4.h 排在 gfx_fb.h 前面，所以定义放这里）。
//
// 为什么是 24 而不是 16：
//   本板的 DSI 流与面板 COLMOD 都是 RGB888（retro-go-tab5 的
//   mipi_dsi_claw4.h 注释写死了"fb 565 + 流 888 只剩 DSI 桥硬件转换
//   一条路，实测那条路是蓝屏（欠载）"）。既然 fb 必须是 888，
//   那就让游戏**直接画 888**——v0.5.0 是"画 565 + 每帧全量展开成
//   888"，那笔展开（518400 次乘除 + 1.55MB 写）是帧时间的主要成分。
//   改成本文件后，它整个消失了。
#define PTD_FB_BPP           24
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

// ── 音频功放：挂在 TCA9555（I2C 0x20）上，**不是** GPIO 直驱 ──────────
// Claw4 的扬声器前面串了一个功放，它的使能脚不在 ESP32-P4 的 GPIO 上，
// 而在板载的 TCA9555 IO 扩展芯片上：
//     PA        = P1.0（io_index 8）  —— 1 = 功放开
//     PA_SWITCH = P0.1（io_index 1）  —— 1 = 音源取 ESP32（0 = 4G 模组）
// 两个都必须对，缺任何一个都是"静音"，而且症状完全一样，看不出是哪个：
//   只开 PA 不选音源  → 功放放大的是 4G 模组那一路（本固件下一直静音）
//   只选音源不开 PA   → 信号到了功放输入，但功放没供电
// PA 还要**延迟开**：官方固件上电后 5s 才开（避免上电瞬间推动放爆音）。
#define CLAW4_IOEXP_ADDR     0x20      // TCA9555，A0=A1=A2=0
#define CLAW4_IOEXP_PA_BIT   8         // io_index（TCA9555 P1.0）
#define CLAW4_IOEXP_PASW_BIT 1         // io_index（TCA9555 P0.1）
#define CLAW4_PA_DELAY_MS    2500      // 上电 → 开功放的间隔
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
// ════════════════════════════════════════════════════════════════════
// Gfx 实现——直接画进 DSI 驱动自己的帧缓冲
// ════════════════════════════════════════════════════════════════════
// v0.5.1 的关键改动：**零转换 + 零拷贝**。
//
// v0.5.0 的管线是"GfxFb 画进一块自分配的 RGB565 → commit 全量展开成
// 一块自分配的 RGB888 → draw_bitmap 把 1.55MB memcpy 进驱动的 fb"。
//
// 读 esp_lcd_panel_dpi.c:520 的 dpi_panel_draw_bitmap_2d 会发现：
//     if (draw_buffer 落在 fbs[i] 的内存区间内)
//         → 不 memcpy，只 esp_cache_msync 回写，并切 cur_fb_index
// 于是只要**画进驱动的 fb**，那两笔开销（全量格式展开 + 整块 memcpy）
// 就一起没了。剩下的只有 msync——而 msync 是无论如何都要做的
// （P4 的 PSRAM 是 cacheable，DSI 的 DPI 硬件直接读物理内存）。
//
// 代价：失去"自己那块 565 缓冲"带来的双缓冲/整屏裁剪自由。但本游戏
// 本来就是单缓冲 + 脏格重绘，不需要。
class GfxClaw4 {
 public:
  // = esp_lcd_dpi_panel_get_frame_buffer() 取回的那一块（不是我们分配的）
  uint8_t*  fb = nullptr;
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
void  ioexpInit(void);
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
