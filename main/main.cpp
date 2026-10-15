/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 andjiang0083
 *
 * main.cpp — PIXEL TOWER DEFENSE · Metalio Claw4 固件入口
 *
 * 与上游 Tab5 版 main.cpp 的对应关系（v0.5.0 / Claw4 移植）：
 *   上游M5.begin/config/setRotation/Preferences  →  GfxClaw4::init / nvsInit
 *   上游 M5.update + M5.Touch.getDetail()        →  touchRead()
 *   上游 M5.Speaker.playRaw + ES8388 寄存器 hack →  audioPlayRaw()
 *
 * 游戏逻辑（game.h / actions.h / render.h / pixel_text.h）与 PC 模拟器跑的是
 * **同一份源码**，一行都没改。这份 main 只做三件事：接线、跑帧、存进度。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include "bsp_claw4.h"

// 游戏源码（与 PC 模拟器共用的同一批header）
#include "config_gen.h"
#include "gfx_fb.h"
#include "pixel_text.h"
#include "i18n.h"
#include "game.h"
#include "render.h"
#include "actions.h"

static const char* TAG = "ptd";

// 全局帧缓冲设备（GfxFb 借用 Claw4 PSRAM，绘制端只管写）
static GfxClaw4 g_bsp;
static GfxFb     g_gfx;

// ── 触摸适配：把 BSP 的原始点喂进 tap-on-release 状态机 ──────────────
// ⚠️ 上游 touch.h 的 readTap() 直接调 M5.Touch，无法复用。这里重写等价的
//   非阻塞逻辑（tap-on-release：记下按下点，抬手时位移小才算点击）。
//   这段逻辑与硬件无关，是上游设计的核心价值，行为保持一致。
static bool tapRead(Tap* t) {
  static bool pressed = false;
  static int16_t px = 0, py = 0;
  static uint32_t downMs = 0;
  t->valid = false; t->x = -1; t->y = -1;

  Claw4TouchPoint p;
  if (touchRead(&p) && p.down) {
    if (!pressed) { pressed = true; px = p.x; py = p.y; downMs = millis(); }
    return false;                       // 还按着：不产生点击，世界继续跑
  }
  if (!pressed) return false;
  pressed = false;

  int dx = p.x - px, dy = p.y - py;
  if (dx*dx + dy*dy > 24*24) return false;      // 大幅拖动 = 误触
  if (millis() - downMs > 1500) return false;   // 长按无响应
  t->valid = true; t->x = px; t->y = py;
  return true;
}

// ── 进度落盘 ─────────────────────────────────────────────────────────
static void saveProgress(void) {
  nvsPutU32("unlock", g_unlocked);
  nvsPutU32("clear",  g_cleared);
  nvsPutU32("stars",  packStars(0));
  nvsPutU32("stars2", packStars(10));
  nvsPutU8("mute",s_muted ? 1 : 0);
  nvsPutU8("onboarded", g_onboarded ? 1 : 0);
  nvsPutU16("endbest", (uint16_t)g_endBest);
  nvsSave();
}

static const uint32_t FRAME_MS = 33;   // 30fps

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "=== PIXEL TOWER DEFENSE v0.5.1 | Claw4 | %dx%d | fb %dbpp ===",
           SCREEN_W, SCREEN_H, PTD_FB_BPP);

  // ── 1. 板级初始化 ────────────────────────────────────────────────
  g_bsp.init();
  g_gfx = GfxFb(g_bsp.fb, false);      // 借用 BSP 的 PSRAM 帧缓冲
  if (!touchInit()) ESP_LOGW(TAG, "touch unavailable — 画面可看，交互不可用");
  audioInit();
  audioSetVolume(70);
  nvsInit();

  ESP_LOGI(TAG, "PSRAM free %d KB, largest block %d KB",
           (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           (int)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));

  // ── 2. 读进度 → 标题界面（与上游 v0.4.x 同一逻辑）───────────────
  g_unlocked   = nvsGetU32("unlock", 0x0001) | 0x0001;
  g_cleared    = nvsGetU32("clear", 0);
  uint32_t pk1 = nvsGetU32("stars", 0);
  uint32_t pk2 = nvsGetU32("stars2", 0);
  sfxSetMuted(nvsGetU8("mute", 0) != 0);
  g_onboarded = nvsGetU8("onboarded", 0) != 0;
  g_endBest   = (int16_t)nvsGetU16("endbest", 0);
  for (int i = 0; i < LEVEL_COUNT && i < 10;++i)  g_stars[i] = (uint8_t)((pk1 >> (i*3)) & 7);
  for (int i = 10; i < LEVEL_COUNT && i < 20;++i) g_stars[i] = (uint8_t)((pk2 >> ((i-10)*3)) & 7);
  g_saveProgress = saveProgress;

  levelLoad(0);
  g_w.state = ST_TITLE;
  drawTitle(&g_gfx);
  g_bsp.commit();       // 首帧立即推送，不等主循环（否则白屏 1 帧）

  ESP_LOGI(TAG, "unlock=%04X cleared=%04X levels=%d",
           g_unlocked, g_cleared, LEVEL_COUNT);

  // ── 3. 主循环（对应上游 loop()）─────────────────────────────────
  static uint32_t lastFrame = 0;
  static int8_t lastOnboard = 0;

  while (1) {
    feedWdt();

    Tap t;
    if (tapRead(&t)) handleTap(&g_gfx, t);

    // 标题 / 选关：静态画面，无世界逻辑
    // ⚠️ draw_bitmap 模式下每帧都要commit。标题/选关界面虽然是静态的，
    // 但 drawTitle() 只在启动时调一次，若这里 continue 掉，帧缓冲永远
    // 停留在初始的黑色—— 屏上就是全黑。
    // 这正是 video mode 与 draw_bitmap 的关键差异：前者 DPI 硬件持续
    // 扫描 fb，应用画完即可；后者必须每帧显式推一次。
    if (g_w.state == ST_TITLE) { g_bsp.commit(); delayMs(20); continue; }
    if (g_w.state == ST_SEL)   { updateSelFx(&g_gfx); g_bsp.commit(); delayMs(20); continue; }

    uint32_t now = millis();
    uint32_t dt = now - lastFrame;
    if (dt < FRAME_MS) { vTaskDelay(1); continue; }
    if (dt > 60) dt = 60;
    lastFrame = now;

    gameUpdate((int)dt);
    renderFrame(&g_gfx);

    // 引导overlay：按 step 变化重画一次（不每帧刷，详见上游 main.cpp 注释里
    // 那个 14400 fillRect/帧 → esp_cache_msync 死锁的真机崩溃）
    if (g_onboardStep > 0 &&
        (g_w.state == ST_READY || g_w.state == ST_WAVE)) {
      if (g_onboardStep != lastOnboard) {
        drawOnboard(&g_gfx, g_onboardStep);
        lastOnboard = g_onboardStep;
      }
    } else {
      lastOnboard = 0;
    }
    updateBuildFx(&g_gfx);
    updateOverlays(&g_gfx);

    // 音效派发在帧间隙（渲染之后）
    s_nowMs = now;
    sfxPump();

    progressTickOnClear();
    g_bsp.commit();

    // ── 帧时间统计（每 60 帧打一次）──────────────────────────────
    // v0.5.1 性能改动要能被量化，不然"变快了"只是感觉。
    // 这里量的是"帧间隔"（含 FRAME_MS 的等待），所以稳态下应接近
    // FRAME_MS；真正要看的是它**超过** FRAME_MS 多少——那部分是
    // 渲染 + commit 的真实成本。
    static uint32_t perfN = 0, perfT0 = 0, perfOver = 0;
    if (perfN == 0) perfT0 = now;
    if (dt > FRAME_MS) perfOver += (dt - FRAME_MS);
    if (++perfN >= 60) {
      ESP_LOGI(TAG, "fps=%.1f  render+commit overrun=%u ms/60f  px/frame=%llu",
               60000.0f / (float)(now - perfT0),
               (unsigned)perfOver,
               (unsigned long long)(GfxFb::pxFilled / 60));
      perfN = 0; perfOver = 0;
      GfxFb::pxFilled = 0;
    }
  }
}
