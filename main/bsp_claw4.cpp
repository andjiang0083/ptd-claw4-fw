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
#include "esp_ldo_regulator.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char* TAG = "ptd";


static i2c_master_bus_handle_t s_i2c_bus = NULL;
static esp_lcd_panel_handle_t  s_panel   = NULL;
static esp_lcd_dsi_bus_handle_t s_dsi_bus = NULL;
static ledc_channel_t          s_bl_chan = LEDC_CHANNEL_0;
static int                     s_backlight = 80;
// 自检开关：默认开（第一屏纯红）。确认链路通了之后改成 false，
// 或者在运行时把它置0 —— 它只是往帧缓冲写个颜色，不影响其他逻辑。
static bool                    s_selftest_pattern = true;

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


  // ── 4. DPI 面板参数（官方 720x720@60Hz 宏）────────────────────
  // 同上：手写厂商宏的等价内容（720x720 @ 60Hz，dpi_clk=36MHz）
  static esp_lcd_dpi_panel_config_t dpi_cfg = {};
  dpi_cfg.dpi_clk_src       = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi_cfg.dpi_clock_freq_mhz = 36;
  dpi_cfg.virtual_channel= 0;
  dpi_cfg.pixel_format    = LCD_COLOR_PIXEL_FORMAT_RGB565;
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
  pdev.bits_per_pixel = 16;
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
  // 这是 video mode 的固有契约：帧缓冲由 DSI 驱动持有，应用只能通过
  // esp_lcd_dpi_panel_get_frame_buffer() 取回。不能自己分配。
  void *dpi_fb = NULL;
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &dpi_fb));
  ESP_ERROR_CHECK(dpi_fb ? ESP_OK : ESP_ERR_NO_MEM);
  fb = (uint16_t*)dpi_fb;
  const size_t fb_bytes = (size_t)CLAW4_LCD_H_RES * CLAW4_LCD_V_RES * 2;
  memset(fb, 0, fb_bytes);
  ESP_LOGI(TAG, "frame buffer from DSI driver: %p (%u bytes)", fb, (unsigned)fb_bytes);

  // ── 6. 上屏 ────────────────────────────────────────────────────
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

  setBrightness(s_backlight);
  // ── 7. 自检标记（可用 PTD_SELFTEST_PATTERN 关闭）───────────────
  //   先往帧缓冲写纯红。若屏幕立刻变红 → 写入路径与扫描路径已对上；
  //   若仍黑 → 问题在 DSI 时序/屏初始化，与游戏代码无关。
  //   这一步把"黑屏"这一个症状拆成了两个可区分的故障域，
  //   比盲调 dpi_clk 有效得多。
  if (s_selftest_pattern) {
    for (size_t i = 0; i < fb_bytes / 2; ++i) fb[i] = 0xF800;   // RGB565 红
    ESP_LOGW(TAG, "SELFTEST: frame buffer filled red");
  }

  ESP_LOGI(TAG, "DSI up: %dx%d RGB565 fb=%p",
           CLAW4_LCD_H_RES, CLAW4_LCD_V_RES, fb);
}

void GfxClaw4::commit(void) {
  // video mode 下帧缓冲常驻扫描，无需逐帧提交。
  // 若真机出现撕裂/花屏，在这里改成 esp_lcd_panel_draw_bitmap 手动刷新。
}

// ============================================================================
// 触摸：GT911（I2C）
// ============================================================================
#define GT911_ADDR      0x5D
// GT911 寄存器地址是 16 位，但 i2c_master_transmit 收的 buf 是 uint8_t*，
// 所以这里只保留低 8 位（高字节是寄存器 bank 标志，本驱动不需要）。
#define GT911_STATUS    ((uint8_t)0x4E)
#define GT911_DATA      ((uint8_t)0x50)
#define GT911_MAX_PT    5

void touchInit(void) {
  i2c_bus_init();
  // INT 低有效 → GPIO 输入 + 下拉
  gpio_config_t icfg = {
    .pin_bit_mask = 1ULL << CLAW4_TOUCH_INT,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,     // GT911 INT 低有效 → 用下拉
    .pull_down_en = GPIO_PULLDOWN_ENABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&icfg));

  // GT911 上电默认寄存器地址；INT 拉一个脉冲让控制器切到 0x5D
  gpio_set_level(CLAW4_TOUCH_INT, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(CLAW4_TOUCH_INT, 1);
  vTaskDelay(pdMS_TO_TICKS(10));

  uint8_t buf[1] = {0};
  i2c_master_dev_handle_t dev = NULL;
  i2c_device_config_t dcfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = GT911_ADDR,
    .scl_speed_hz    = 400000,
  };
  if (i2c_master_bus_add_device(s_i2c_bus, &dcfg, &dev) == ESP_OK) {
    // GT911 reset: 写 0 到 0x2024 寄存器
    uint8_t rst[2] = {0x20, 0x24};
    i2c_master_transmit(dev, rst, 2, 100);
    vTaskDelay(pdMS_TO_TICKS(10));
    i2c_master_transmit(dev, (uint8_t[]){0x00}, 1, 100);
  }
  ESP_LOGI(TAG, "GT911 init done");
}

// GT911 每点8 字节：track_id(1) x(2) y(2) size(2) reserved(1)
static uint16_t gt911_reg_read16(uint8_t reg) {
  uint8_t buf[2] = {reg, 0};
  i2c_master_dev_handle_t dev = NULL;
  i2c_device_config_t dcfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = GT911_ADDR,
    .scl_speed_hz    = 400000,
  };
  if (i2c_master_bus_add_device(s_i2c_bus, &dcfg, &dev) != ESP_OK) return 0;
  i2c_master_transmit_receive(dev, buf, 1, buf, 2, 100);
  i2c_master_bus_rm_device(dev);
  return (uint16_t)(buf[0] | (buf[1] << 8));
}

bool touchRead(Claw4TouchPoint* out) {
  uint16_t status = gt911_reg_read16(GT911_STATUS);
  if ((status & 0x8000) == 0) {          // 无新数据
    out->down = false;
    return false;
  }
  uint8_t hdr[3];
  uint8_t h[3] = {GT911_DATA, 0, 0};
  i2c_master_dev_handle_t dev = NULL;
  i2c_device_config_t dcfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = GT911_ADDR,
    .scl_speed_hz    = 400000,
  };
  if (i2c_master_bus_add_device(s_i2c_bus, &dcfg, &dev) != ESP_OK)
    return false;
  i2c_master_transmit_receive(dev, h, 1, hdr, 3, 100);
  uint8_t npts = hdr[0];
  if (npts == 0 || npts > GT911_MAX_PT) {
    i2c_master_bus_rm_device(dev);
    return false;
  }
  uint8_t pt[8] = {GT911_DATA, 0, 0};
  uint8_t data[8] = {0};
  i2c_master_transmit_receive(dev, pt, 1, data, 8, 100);
  i2c_master_bus_rm_device(dev);

  // 清数据有效位
  uint8_t clr[2] = {GT911_STATUS, 0};
  i2c_master_bus_add_device(s_i2c_bus, &dcfg, &dev);
  i2c_master_transmit(dev, clr, 2, 100);
  i2c_master_bus_rm_device(dev);

  uint16_t x = data[1] | (data[2] << 8);
  uint16_t y = data[3] | (data[4] << 8);
  // GT911 原生分辨率通常是 720x720；若 config.h 的 SWAP_XY 生效需交换
  out->x = x; out->y = y;
  out->down = true;
  return true;
}

int touchWidth(void)  { return CLAW4_LCD_H_RES; }
int touchHeight(void) { return CLAW4_LCD_V_RES; }

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
      .ws   = I2S_GPIO_UNUSED,
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
