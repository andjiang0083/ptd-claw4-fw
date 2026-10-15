/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 andjiang0083
 *
 * bsp_claw4.c — Metalio Claw4 板级实现
 */

#include "bsp_claw4.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_cache.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"      // I2S 标准模式（音效PCM 直写）
#include "driver/i2s_common.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"     // esp_lcd_panel_disp_on_off/reset/init
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_nv3051f.h"
#include "esp_lcd_io_i2c.h"          // esp_lcd_new_panel_io_i2c_v2（不在 esp_lcd_panel_io_i2c.h）
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "nvs_flash.h"
#include "nvs.h"

// 游戏渲染内核：本文件要用 GfxFb 的脏矩形（GfxFb::drY0 等静态成员）。
// 放在 bsp_claw4.h 之后 —— PTD_FB_BPP 由它定义，gfx_fb.h 依赖这个宏。
#include "gfx_fb.h"

static const char* TAG = "ptd";


static i2c_master_bus_handle_t s_i2c_bus = NULL;
static esp_lcd_panel_handle_t  s_panel   = NULL;
static esp_lcd_panel_io_handle_t s_lcd_io = NULL;   // 保留以便手动补发 COLMOD/MADCTL
static esp_lcd_touch_handle_t   s_touch   = NULL;   // GT911
static bool                     s_touch_dev = false;
static esp_lcd_dsi_bus_handle_t s_dsi_bus = NULL;
static ledc_channel_t          s_bl_chan = LEDC_CHANNEL_0;
static int                     s_backlight = 80;
// 自检开关：默认开（第一屏纯红）。确认链路通了之后改成 false，
// 或者在运行时把它置0 —— 它只是往帧缓冲写个颜色，不影响其他逻辑。
static bool                    s_selftest_pattern = false;
static size_t                   fb_bytes_cached    = 0;

// ============================================================================
// 杂项
// ============================================================================
uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

void delayMs(uint32_t ms) {
  // ⚠️ 用 esp_timer 原始计数而非 millis()：后者是 uint32_t 回绕的，
  //   跨 49.7 天边界时会死循环（虽然本固件跑不了那么久，但这是好习惯）。
  const int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
  while (esp_timer_get_time() < until) {
    vTaskDelay(pdMS_TO_TICKS(1));
    feedWdt();
  }
}

// ⚠️ v0.5.0 真机踩坑（闪屏 → 满屏日志 → 渲染饿死）：
//   esp_task_wdt_reset() 只能由**已订阅** WDT 的任务调用，否则返回
//   ESP_ERR_NOT_FOUND 并由 IDF 打一条 E/W 级日志。app_main 是普通任务，
//   默认不在订阅名单里（CONFIG_ESP_TASK_WDT_INIT 只自动订阅 idle），
//   于是主循环每帧都触发一条错误日志 —— 串口刷屏的速率足以让渲染
//   饿死，表现为画面闪烁无内容。
//
// 正确做法：在 init 里显式订阅，feedWdt 保持无条件调用（语义仍清晰），
// 并且**只在第一次订阅失败时降级为静默**，避免日志风暴掩盖真问题。
static bool s_wdt_ok = false;

static void wdt_subscribe(void) {
  esp_err_t err = esp_task_wdt_add(NULL);
  if (err == ESP_OK) {
    s_wdt_ok = true;
    ESP_LOGI(TAG, "WDT subscribed (app_main)");
  } else if (err == ESP_ERR_INVALID_STATE) {
    // 已订阅（重复 init），视为成功
    s_wdt_ok = true;
  } else {
    ESP_LOGW(TAG, "WDT subscribe failed (%s); feedWdt disabled", esp_err_to_name(err));
    s_wdt_ok = false;
  }
}

void feedWdt(void) {
  // 未订阅时静默返回：WDT 是"保险丝"，不是功能。宁可不喂也不能刷屏。
  if (s_wdt_ok) esp_task_wdt_reset();
}

// ============================================================================
// I2C（GT911 用）
// ============================================================================
static void i2c_bus_init(void) {
  if (s_i2c_bus) return;
  i2c_master_bus_config_t cfg = {};
  cfg.i2c_port= CLAW4_I2C_PORT;
  cfg.sda_io_num = CLAW4_I2C_SDA;
  cfg.scl_io_num = CLAW4_I2C_SCL;
  cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  cfg.glitch_ignore_cnt          = 7;
  cfg.flags.enable_internal_pullup = true;
  ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_i2c_bus));
}

// ============================================================================
// 背光（GPIO52 LEDC PWM）
// ============================================================================
static void backlight_init(void) {
  ledc_timer_config_t tc = {
    .speed_mode       = LEDC_LOW_SPEED_MODE,
    .duty_resolution  = LEDC_TIMER_10_BIT,
    .timer_num        = LEDC_TIMER_0,
    .freq_hz         = 5000,
    .clk_cfg         = LEDC_AUTO_CLK,
  };
  ESP_ERROR_CHECK(ledc_timer_config(&tc));
  ledc_channel_config_t cc = {
    .gpio_num   = CLAW4_BL_PWM_PIN,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel    = LEDC_CHANNEL_0,
    .timer_sel  = LEDC_TIMER_0,
    .duty       = 0,
    .hpoint     = 0,
  };
  ESP_ERROR_CHECK(ledc_channel_config(&cc));

}

void GfxClaw4::setBrightness(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  s_backlight = pct;
  // 10bit 分辨率 → 0..1023；PWM 可能反相（config.h: DISPLAY_BACKLIGHT_OUTPUT_INVERT false）
  uint32_t duty = (uint32_t)((1023 * pct) / 100);
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, s_bl_chan, duty));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, s_bl_chan));
}

// ============================================================================
// 显示：DSI + NV3051F + PSRAM 帧缓冲（video mode）
// ============================================================================
// 架构与 Tab5 完全一致：帧缓冲常驻 PSRAM，由 DSI DMA 连续扫描输出。
// 绘制端（gfx_fb.h 的 GfxFb）只管往 fb 里写，不需要知道 DSI 的存在。
//
// ⚠️ video mode 的关键约束（Tab5 上踩过，Claw4 同样适用）：
//   PSRAM 会被 DSI DMA 高频读取。如果 fb 所在内存被 CPU cache 持有而没有
//   正确配置成 DMA 可访问属性，会出现撕裂甚至 esp_cache_msync 争用死锁。
//   esp_lcd_new_rgb_dbi_panel 传 fb 时会自己处理 framebuffer 属性，我们只负责
//   分配时用 MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA（见下）。
void GfxClaw4::init(void) {
  wdt_subscribe();
  i2c_bus_init();
  backlight_init();

  // ── 1. DSI PHY 供电（LDO 通道 3 @ 2500mV；不配屏不会亮）────────
  //⚠️ API 说明：esp_ldo_acquire_channel 的第二参是**通道句柄**输出，
  //    5.5 里类型是 esp_ldo_channel_handle_t（不是旧版的枚举）。
  esp_ldo_channel_config_t ldo = {};
  ldo.chan_id    = CLAW4_DSI_LDO_CHAN;
  ldo.voltage_mv = CLAW4_DSI_LDO_MV;
  esp_ldo_channel_handle_t ldo_h = (esp_ldo_channel_handle_t)0;
  ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo, &ldo_h));
  ESP_LOGI(TAG, "DSI LDO chan%d @%dmV acquired", CLAW4_DSI_LDO_CHAN, CLAW4_DSI_LDO_MV);

  // ── 2. DSI 总线 + DBI IO（官方宏，来自 esp_lcd_nv3051f.h）────────
  //    宏里的 lane_rate = 1000 Mbps、dpi_clk = 36MHz 就是 Claw4 官方固件
  //    实测能亮屏的参数，不要自己改成"更大的数试试"——DPI 时序与 DSI 速率
  //    必须匹配，改了大概率黑屏或花屏。
  static nv3051f_vendor_config_t vendor = {};
  vendor.mipi_config.dsi_bus = nullptr;      // 在 new_dsi_bus 之后回填
  // ⚠️ 这里手写而非用 NV3051F_PANEL_BUS_DSI_2CH_CONFIG 宏：厂商头里的宏按
   //    较新的 IDF 写（phy_clk_src=0 裸赋值不能隐式转枚举），在本机 IDF 5.5.2
   //    编译不过。数值与宏完全一致（2 lane / 1000 Mbps），是官方实测参数。
  static esp_lcd_dsi_bus_config_t dsi_cfg = {};
  dsi_cfg.bus_id           = 0;
  dsi_cfg.num_data_lanes   = CLAW4_DSI_LANES;
  dsi_cfg.phy_clk_src      = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
  dsi_cfg.lane_bit_rate_mbps = 1000;   // IDF 5.5 字段名（6.0 起为 lane_rate_mbps）
  ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&dsi_cfg, &s_dsi_bus));
  vendor.mipi_config.dsi_bus = s_dsi_bus;

  static esp_lcd_dbi_io_config_t dbi_cfg = {};
  dbi_cfg.virtual_channel = 0;
  dbi_cfg.lcd_cmd_bits    = 8;
  dbi_cfg.lcd_param_bits  = 8;
  esp_lcd_panel_io_handle_t io = NULL;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &io));
  s_lcd_io = io;


  // ── 4. DPI 面板参数（官方 720x720@60Hz 宏）────────────────────
  // 同上：手写厂商宏的等价内容（720x720 @ 60Hz，dpi_clk=36MHz）
  static esp_lcd_dpi_panel_config_t dpi_cfg = {};
  dpi_cfg.dpi_clk_src       = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi_cfg.dpi_clock_freq_mhz = 48;   // 见下：厂商基线是 48，不是 36
  dpi_cfg.virtual_channel= 0;
  // ⚠️ v0.5.0 真机"颜色全乱"的诊断（此处踩了一个非常隐蔽的坑）：
  //
  // esp_lcd_dpi_panel_config_t 有三个格式字段，但驱动里的判断是
  //     if (panel_config->in_color_format) { ... }
  // 而 LCD_COLOR_FMT_RGB565 的**数值恰好是 0**（COLOR_TYPE_ID 把
  // COLOR_SPACE_RGB=0 和 COLOR_PIXEL_RGB565=0 拼成 0x00000000）。
  // 于是 "in_color_format = RGB565" 等价于"没设置"，
  // 赋值被if 判断静默丢弃 —— 编译器不报错，日志无警告，
  // 画面照旧全错。我上一轮就是被这个坑了一轮。
  //
  // 因此唯一的可靠入口是 pixel_format（它走 switch，不依赖真假判断）。
  // 这里设 RGB888：驱动按 24bpp 分配 fb，DSI 侧按 24bpp 输出，
  // 面板的 COLMOD 由驱动设为 0x77（RGB888）。三者自然一致。
  // 代价是 fb 从 1.03MB 涨到 1.48MB —— 32MB PSRAM 完全够。
  //
  // 代价二：GfxFb 与游戏代码全部按 RGB565 写（uint16_t），
  //   这里必须做一次 RGB565→RGB888 转换。放在驱动侧不可能
  //   （应用只能拿到裸 fb 指针），所以放在 GfxFb 的实现里。
  dpi_cfg.pixel_format    = LCD_COLOR_PIXEL_FORMAT_RGB888;
  dpi_cfg.out_color_format = LCD_COLOR_FMT_RGB888;  // 与 fb 一致 = DSI 桥不做转换
  dpi_cfg.num_fbs         = 1;
  dpi_cfg.video_timing.h_size           = CLAW4_LCD_H_RES;
  dpi_cfg.video_timing.v_size           = CLAW4_LCD_V_RES;
  dpi_cfg.video_timing.hsync_back_porch = 44;
  dpi_cfg.video_timing.hsync_pulse_width= 2;
  dpi_cfg.video_timing.hsync_front_porch= 46;
  dpi_cfg.video_timing.vsync_back_porch = 14;
  dpi_cfg.video_timing.vsync_pulse_width= 2;
  dpi_cfg.video_timing.vsync_front_porch= 16;
  vendor.mipi_config.dpi_config = &dpi_cfg;

  // ── 5. 面板（NV3051F：带厂商 init command 序列）───────────────
  esp_lcd_panel_dev_config_t pdev = {};
  pdev.reset_gpio_num = CLAW4_LCD_RST_PIN;
  pdev.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
  pdev.bits_per_pixel = 24;   // → colmod 0x77（RGB888），与 DSI 流一致
  pdev.vendor_config  = &vendor;
  ESP_ERROR_CHECK(esp_lcd_new_panel_nv3051f(io, &pdev, &s_panel));
  panel_ = s_panel;

  // ⚠️ v0.5.0 真机黑屏根因：帧缓冲必须用 **驱动分配的那一块**。
  //
  // 之前我在 PSRAM 里自己 malloc 了 1.03MB，然后交给 GfxFb 画。但
  // esp_lcd_new_panel_dpi() **自己**又分配了一块（见 esp_lcd_panel_dpi.c:233，
  // 同样是 PSRAM | MALLOC_CAP_DMA），DPI 硬件扫描的是它，不是我的那块。
  // 于是整个游戏把画面画在一块从未被扫描的内存里 —— 初始化全绿
  //（DSI up / GT911 / audio init都成功），但屏幕永远黑。
  //
  // v0.5.1：**拿驱动那块自己用的帧缓冲来画**。
  //
  // 之前（v0.5.0）是"自己 malloc 565 → 每帧展开成自己 malloc 的 888
  // → draw_bitmap 把 1.55MB memcpy 进驱动那块"。三份缓冲、两笔全屏开销。
  //
  // 现在只留一份：驱动自己的那块。draw_bitmap 传进去的指针落在
  // fbs[0] 区间内，驱动就会走 esp_lcd_panel_dpi.c 里那条
  // "do_copy = false → 只 msync" 的分支（见该文件 520 行附近）。
  // 于是每帧只剩：绘制本身 + msync 脏行。
  {
    void* pfb = nullptr;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &pfb));
    fb = (uint8_t*)pfb;
    ESP_ERROR_CHECK(fb ? ESP_OK : ESP_ERR_NO_MEM);
    // 驱动只保证"这块内存存在"，不保证内容。清一次，避免上电花屏。
    fb_bytes_cached = (size_t)CLAW4_LCD_H_RES * CLAW4_LCD_V_RES * 3;
    memset(fb, 0, fb_bytes_cached);
    ESP_LOGI(TAG, "fb=%p (%u B, driver-owned)  [RGB888 zero-conversion]",
             fb, (unsigned)fb_bytes_cached);
  }

  // ── 6. 上屏 ────────────────────────────────────────────────────
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

  // ⚠️ 补发 COLMOD(0x3A) —— 厂商驱动的确凿缺陷。
  //
  // esp_lcd_nv3051f.c 里 colmod_val 被 new_panel 依据 bits_per_pixel
  // 算成 0x55（RGB565）、init 循环又反复赋值，但**全文没有任何一处
  // 把它发出去**：0x3A 从未出现在 tx_param 调用里。驱动只"记得"
  // 却"没说"。于是面板一直停在各自的上电默认 COLMOD。
  //
  // 官方固件不走这个驱动（它有自己的显示栈），所以那边看不出问题。
  // 这里显式补上，让面板与帧缓冲格式对齐。
  {
    const uint8_t colmod = 0x77;      // RGB888（与 fb 和 DSI 流一致）
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x3A, &colmod, 1));
    ESP_LOGI(TAG, "COLMOD(0x3A) = 0x77 (RGB888) [patched: vendor driver never sends it]");
  }

  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

  setBrightness(s_backlight);
  ESP_LOGI(TAG, "DSI up: %dx%d RGB888 fb=%p", CLAW4_LCD_H_RES, CLAW4_LCD_V_RES, fb);
}

// ⚠️ v0.5.0 真机"文字残缺、背景正常"根因：PSRAM cache 未回写。
//
// 机制：P4 的 PSRAM 是 cacheable 的。CPU 写帧缓冲先进cache，
//   而 DSI 的 DPI 硬件是 DMA，**直接读物理内存**、看不见 cache。
//   于是 CPU 认为"画完了"，屏上却还是旧内容。
//
// 为什么症状是"残缺"而不是"全黑"：cache line 只在被逐出时才写回。
//   其他代码访问内存会让部分 line 意外evict，于是画面呈现
//   "一部分更新了、一部分还是初始值"——正好是看到的残缺字形。
//
// 为什么官方固件没这问题：驱动自己的 draw_bitmap 路径在
//   esp_lcd_panel_dpi.c:518/541/555 每次都esp_cache_msync(C2M)，
//   而 video mode 只在 init 时msync 一次（:241），之后把责任交给应用。
//   官方 Claw4 固件走自己的显示栈，天然带这一步；我直接用
//   esp_lcd_panel_dpi 的 video mode，就漏了。
//
// 代价：每帧 1.55MB 写回（RGB888）。P4 的 PSRAM 200MHz 双倍带宽足够，
//   但它毕竟是每帧固定支出 —— v0.5.1 因此给 GfxFb 加了脏矩形跟踪，
//   只 msync 变化的那几行（见下面的 commit）。
//
// ── 提交一帧 ────────────────────────────────────────────────────────
// v0.5.1：不再有任何格式转换与整块拷贝。
//   游戏已经把 RGB888 直接画进了驱动那块帧缓冲，这里只做两件事：
//     ① 按脏矩形算出要回写的扫描行区间
//     ② draw_bitmap 让驱动 msync 这些行（它识别出指针是自己人，不会 memcpy）
//
// ⚠️ msync 的粒度是**整行**：esp_lcd_panel_dpi.c 里是
//     cache_sync_start = fbs[i] + y_start * h_pixels * bpp/8
//     cache_sync_size  = (y_end - y_start) * h_pixels * bpp/8
//   它不认 x。所以脏矩形的 x 范围在这里用不上（保留给将来做列级优化），
//   真正省时间的是 y 范围——战斗中通常只有地图中间那些行在动。
//
// ⚠️ 为什么必须有"脏矩形为空就跳过"这一条：
//   标题/选关是静态界面，主循环里每 20ms 都会调 commit。没有这一条，
//   静态界面也在每帧 msync 全屏 1.55MB，纯属白烧。
void GfxClaw4::commit(void) {
  if (!s_panel || !fb) return;
  if (GfxFb::dirtyEmpty()) return;              // 本帧什么都没画

  int x0 = GfxFb::drX0, y0 = GfxFb::drY0;
  int x1 = GfxFb::drX1, y1 = GfxFb::drY1;
  GfxFb::dirtyReset();                          // 先取再清，避免竞态

  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > CLAW4_LCD_H_RES - 1) x1 = CLAW4_LCD_H_RES - 1;
  if (y1 > CLAW4_LCD_V_RES - 1) y1 = CLAW4_LCD_V_RES - 1;

  // draw_bitmap 的窗口是半开区间 [x_start, x_end)
  esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1 + 1, y1 + 1, fb);
}

// ============================================================================
// 触摸：GT911（I2C）
// ============================================================================
// 参数照搬官方板级源码（MetalioClaw4 的 InitializeTouch），
// 并与同作者 retro-go-tab5 claw4 分支（rg_input.c: rg_touch_init）对齐：
//
//  ① I2C 地址必须**探测**：GT911 的地址取决于 INT 脚上电时的电平，
//     两个都可能是 0x5D 或 0x14。官方与 retro-go 都是先 0x5D、
//     probe 失败再 0x14。我此前硬编码 0x5D → 屏不亮时它其实在 0x14。
//  ② INT = GPIO33、**低有效**、板上未接 RST。
//     我此前把 INT 配成输出并做脉冲复位——那是错的：INT 是屏主动拉低的
//     中断线，驱动它等于跟屏抢总线。
//  ③ I2C 速率 200kHz（与电量计 BQ27220 同总线；官方注释：400k 在
//     总线抖动时更容易 clear-bus 失败）。我此前用 400k。
//  ④ 复用**显示驱动已建好的 I2C 总线**。官方板级注释明确警告：
//     "GT911 与 TCA9555 若各建各的 I2C 外设会抢总线 ⇒ 通信失败。"
//     我此前在 touchRead 路径里反复 i2c_master_bus_add_device，
//     既泄漏句柄又制造多设备抢总线。
//
// 用官方 esp_lcd_touch_gt911 组件而不是手写寄存器读写：手写要自己对
// 0x814E/0x8150 这类 16 位寄存器地址做 bank 选择，容易错；
// 组件已经处理了这些细节。
bool touchInit(void) {
  if (s_touch_dev) return true;
  i2c_bus_init();
  if (!s_i2c_bus) { ESP_LOGE(TAG, "touch: I2C bus unavailable"); return false; }

  // ① 地址探测
  uint8_t addr = 0x5D;
  if (i2c_master_probe(s_i2c_bus, 0x5D, 100) != ESP_OK) {
    if (i2c_master_probe(s_i2c_bus, 0x14, 100) == ESP_OK) {
      addr = 0x14;
    } else {
      ESP_LOGE(TAG, "touch: GT911 no response on 0x5D/0x14");
      return false;
    }
  }
  ESP_LOGI(TAG, "touch: GT911 @0x%02X", addr);

  esp_lcd_panel_io_handle_t io = NULL;
  // 注意字段名：IDF 5.5 的 esp_lcd_panel_io_i2c_config_t 用 dev_addr
  // （不是 device_address），且**没有** dev_addr_length ——
  // 地址长度由 esp_lcd_new_panel_io_i2c_v2 固定为 7-bit。
  esp_lcd_panel_io_i2c_config_t io_cfg = {};
  io_cfg.dev_addr            = addr;
  io_cfg.control_phase_bytes = 1;
  io_cfg.dc_bit_offset       = 0;
  io_cfg.lcd_cmd_bits        = 16;
  io_cfg.scl_speed_hz        = 200000;      // ③
  io_cfg.flags.disable_control_phase = 1;
  esp_err_t err = esp_lcd_new_panel_io_i2c_v2(s_i2c_bus, &io_cfg, &io);
  if (err != ESP_OK) { ESP_LOGE(TAG, "touch: i2c io failed %s", esp_err_to_name(err)); return false; }

  esp_lcd_touch_config_t tp = {};
  tp.x_max = CLAW4_LCD_H_RES;              // 720，方屏
  tp.y_max = CLAW4_LCD_V_RES;
  tp.rst_gpio_num = (gpio_num_t)-1;          // ② 板上未接 RST
  tp.int_gpio_num = (gpio_num_t)CLAW4_TOUCH_INT;   // 33
  tp.levels.reset      = 0;
  tp.levels.interrupt  = 0;                 // INT 低有效
  tp.flags.swap_xy = 0;
  tp.flags.mirror_x = 0;
  tp.flags.mirror_y = 0;
  err = esp_lcd_touch_new_i2c_gt911(io, &tp, &s_touch);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "touch: gt911 init failed %s", esp_err_to_name(err));
    esp_lcd_panel_io_del(io);
    return false;
  }
  s_touch_dev = true;
  ESP_LOGI(TAG, "touch: GT911 ready (INT 33, 200kHz, no RST)");
  return true;
}

// ⚠️ v0.5.0 真机"画面正常但点不动"根因，两个 bug 叠在一起：
//
// ① esp_lcd_touch_read_data() **只负责读**，它把数据存进驱动的内部
//    状态；坐标必须再调 esp_lcd_touch_get_data() 取。
//    我原先在 read_data() 之后直接读一个**局部空结构体** td
//    （esp_lcd_touch_data_t td = {}）—— 那玩意永远是空的，
//    td.points 恒为 0 ⇒ 函数永远返回 false ⇒ 上层 tapRead 认为
//    "从没按下过" ⇒ 点击全部丢弃。
//
// ② out->down 从未赋值。tapRead() 判断的是 p.down，
//    而 Claw4TouchPoint 是调用方栈上的未初始化变量，
//    down 是随机垃圾值。即使 ① 修好了，② 也会让点击随机失效。
//
// 两个 bug 的症状完全相同（点不动），且都不产生任何编译警告或
// 运行时日志 —— 因为 GT911 初始化是成功的，串口照样打
// "touch: GT911 ready"，看起来一切都对。
//
// 修法：read_data 后用 get_data 取真实坐标，并显式给 down 赋值。
bool touchRead(Claw4TouchPoint* out) {
  if (!s_touch) return false;
  if (esp_lcd_touch_read_data(s_touch) != ESP_OK) return false;

  esp_lcd_touch_point_data_t pts[1] = {};
  uint8_t cnt = 0;
  if (esp_lcd_touch_get_data(s_touch, pts, &cnt, 1) != ESP_OK) return false;

  out->down = (cnt >= 1);          // ② 显式赋值，不留随机值
  if (out->down) {
    out->x = pts[0].x;
    out->y = pts[0].y;
  }
  return out->down;
}

// ============================================================================
// 音频功放（PA）：TCA9555 IO 扩展 —— 没它就没有声音
// ============================================================================
// 为什么必须单独驱动这块芯片：
//   Claw4 的扬声器前面有功放，使能脚挂在板载 TCA9555 上，不经过 ESP32-P4
//   的 GPIO。只把 I2S 配好是"信号到了功放门口"，功放没开 = 一声不响。
//
// 芯片是 16 位 IO 扩展，两个 8 位端口：
//   0x02/0x03 = 输出寄存器（port0 / port1）
//   0x06/0x07 = 方向寄存器（port0 / port1，1=输入 0=输出）
// 写方向寄存器时带 auto-increment，一次写 2 字节就配完两个端口。
//
// ⚠️ 不做"读-改-写"：本固件不碰 GPS / 4G / 蓝牙 / SD 那些脚，把它们留在
//   高阻（方向=输入）最安全 —— 写 0 反而会主动拉低、可能给不该上电的
//   模块上电。只驱动我们自己要用的两根线。
#define TCA9555_REG_OUT0 0x02
#define TCA9555_REG_CFG0 0x06

static i2c_master_dev_handle_t s_ioexp = NULL;

static esp_err_t ioexpWrite(uint8_t reg, const uint8_t* d, size_t n) {
  uint8_t buf[4];
  buf[0] = reg;
  memcpy(buf + 1, d, n);
  return i2c_master_transmit(s_ioexp, buf, n + 1, 100);
}

// bit 编号是 TCA9555 的 io_index（0..7 = P0.0..P0.7，8..15 = P1.0..P1.7）
static esp_err_t ioexpSet(uint8_t io_index, bool high) {
  // 方向：目标位 0（输出），其余 1（输入/高阻）
  uint8_t cfg[2];
  if (io_index < 8) { cfg[0] = (uint8_t)~(1u << io_index); cfg[1] = 0xFF; }
  else              { cfg[0] = 0xFF; cfg[1] = (uint8_t)~(1u << (io_index - 8)); }
  esp_err_t e = ioexpWrite(TCA9555_REG_CFG0, cfg, 2);
  if (e != ESP_OK) return e;
  // 输出：目标位 = level，其余写 0（它们是输入，写了也不驱动）
  uint8_t out[2] = {0, 0};
  if (io_index < 8) out[0] = (uint8_t)(high ? (1u << io_index) : 0);
  else              out[1] = (uint8_t)(high ? (1u << (io_index - 8)) : 0);
  return ioexpWrite(TCA9555_REG_OUT0, out, 2);
}

void ioexpInit(void) {
  i2c_bus_init();
  if (!s_i2c_bus) { ESP_LOGE(TAG, "ioexp: I2C bus unavailable"); return; }
  // probe 先行：老批次板子可能没有这颗芯片，不能让缺芯片变成开机崩溃
  if (i2c_master_probe(s_i2c_bus, CLAW4_IOEXP_ADDR, 100) != ESP_OK) {
    ESP_LOGW(TAG, "TCA9555 not found at 0x%02X — PA 无法使能，将没有声音",
             CLAW4_IOEXP_ADDR);
    return;
  }
  i2c_device_config_t dev = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = CLAW4_IOEXP_ADDR,
    .scl_speed_hz    = 100000,
  };
  if (i2c_master_bus_add_device(s_i2c_bus, &dev, &s_ioexp) != ESP_OK) {
    ESP_LOGE(TAG, "ioexp: add device failed");
    return;
  }
  // 音源选择：1 = 取 ESP32 这一路（0 = 4G 模组）。
  // 这一根和 PA 是"与"关系，漏了它一样没声。
  ioexpSet(CLAW4_IOEXP_PASW_BIT, true);
  ioexpSet(CLAW4_IOEXP_PA_BIT, false);          // PA 先关着
  ESP_LOGI(TAG, "TCA9555 ok @0x%02X  PA_SWITCH=1 (音源=ESP32), PA=0 (待延迟开)",
           CLAW4_IOEXP_ADDR);
  // 延迟开 PA：上电瞬间不推动放，避免"啪"一声爆音
  xTaskCreate([](void*) {
    vTaskDelay(pdMS_TO_TICKS(CLAW4_PA_DELAY_MS));
    ioexpSet(CLAW4_IOEXP_PA_BIT, true);
    ESP_LOGI(TAG, "PA enabled after %d ms boot delay", (int)CLAW4_PA_DELAY_MS);
    vTaskDelete(nullptr);
  }, "pa_enable", 2048, nullptr, 5, nullptr);
}

// ============================================================================
// 音频：I2S 直写 PCM（绕过 codec 抽象，最小可用路径）
// ============================================================================
// sfx_data.h 是 16kHz 单声道 int16 PCM（= CLAW4_SPK_SR），格式天然对齐。
// 这里用 I2S 标准模式直写；Claw4 固件的 AudioCodec/BTAudioCodec 走的是同一
// 组 GPIO，若日后发现路由不对，改成esp_codec_dev 的 AudioCodec 接口即可
// （s_hdl 换成 esp_codec_dev_open 的返回值，SFX_PLAY 逻辑不变）。
static i2s_chan_handle_t s_i2s_tx = NULL;
static int s_volume = 200;              // 0..255，对齐上游 M5.Speaker.setVolume(200)

void audioInit(void) {
  i2s_chan_config_t chan_cfg =
    I2S_CHANNEL_DEFAULT_CONFIG(CLAW4_I2S_PORT, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 4;
  chan_cfg.dma_frame_num = 256;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL));

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CLAW4_SPK_SR),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                   I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = CLAW4_I2S_BCLK,
      // ⚠️ WS **不是** UNUSED：Claw4 的 SPK 只有 BCLK/DOUT 两根线，
      //   WS 与 MIC 共用 GPIO10（官方板文件里 BTAudioCodecDuplex 就是
      //   (bclk=12, ws=10, dout=9, din=11) 这么传的）。
      //   之前这里写 I2S_GPIO_UNUSED ⇒ 有数据、没帧同步 ⇒ 功放收到
      //   一串对不上边界的位流 ⇒ 一声不响。
      .ws   = CLAW4_I2S_MIC_WS,
      .dout  = CLAW4_I2S_DOUT,
      .din   = I2S_GPIO_UNUSED,
      .invert_flags = { false, false, false },
    },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));
  ESP_LOGI(TAG, "audio init ok (I2S%d BCLK=%d DOUT=%d @%dHz)",
           CLAW4_I2S_PORT, CLAW4_I2S_BCLK, CLAW4_I2S_DOUT, CLAW4_SPK_SR);
}

void audioSetVolume(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  s_volume = (pct * 255) / 100;
}

void audioPlayRaw(const int16_t* data, size_t len, uint32_t sr) {
  if (!s_i2s_tx || s_volume <= 0) return;
  (void)sr;                              // 标准配置固定 16kHz
  size_t bytes = len * sizeof(int16_t);
  size_t written = 0;
  // 200ms 超时：音效是短脉冲，卡住就丢，绝不阻塞主循环
  if (i2s_channel_write(s_i2s_tx, data, bytes, &written, 200) != ESP_OK) {
    ESP_LOGD(TAG, "audio write timeout, drop %d bytes", (int)(bytes - written));
  }
}

// ============================================================================
// NVS
// ============================================================================
static const char* NS = "td";           // 与上游 Preferences.begin("td") 同命名空间
static nvs_handle_t s_nvs = 0;

void nvsInit(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  nvs_open(NS, NVS_READWRITE, &s_nvs);
}

void   nvsSave(void)                { nvs_commit(s_nvs); }
uint32_t nvsGetU32(const char* k, uint32_t d) { uint32_t v = d; nvs_get_u32(s_nvs, k, &v); return v; }
uint16_t nvsGetU16(const char* k, uint16_t d) { uint16_t v = d; nvs_get_u16(s_nvs, k, &v); return v; }
uint8_t  nvsGetU8(const char* k, uint8_t d)  { uint8_t  v = d; nvs_get_u8(s_nvs, k, &v);  return v; }
void nvsPutU32(const char* k, uint32_t v) { nvs_set_u32(s_nvs, k, v); }
void nvsPutU16(const char* k, uint16_t v) { nvs_set_u16(s_nvs, k, v); }
void nvsPutU8(const char* k, uint8_t v)   { nvs_set_u8(s_nvs, k, v);   }
