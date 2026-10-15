# PIXEL TOWER DEFENSE · Metalio Claw4 固件

把 pixel-tower-defense（为M5Stack Tab5 1280×720 写的塔防）移植到
nologotech Metalio Claw4（3.95" MIPI-DSI **720×720** 方形屏）。

```
构建：idf.py build
烧录：idf.py -p /dev/tty.usbmodem* flash monitor
产物：build/ptd_claw4.bin（app 348KB，分区剩 89%）
      build/ptd_claw4_merged.bin（合并镜像 413KB，含 bootloader+分区表+app）
```

⚠️ **这是独立固件，会整机覆盖 Claw4 原有的 xiaozhi 固件。**
刷之前确认原固件能重新烧录（`https://github.com/CloudZao/MetalioClaw4`
的 `Metalio_Claw4_Latest.bin` 即为原厂镜像）。

---

## 目录结构

```
main/
  bsp_claw4.h / .cpp    板级支持：DSI 显示 / GT911 触摸 / I2S 音频 / NVS / 背光
  esp_lcd_nv3051f.c/.h  Claw4 官方 NV3051F 驱动（含厂商 init 序列，勿改）
  main.cpp              固件入口（对应上游 Tab5 版的 main.cpp）
  idf_component.yml     托管组件依赖
  CMakeLists.txt
gen/config_gen.h        ← 由 pixel-tower-defense/tools/layout.py --profile claw4 生成
partitions.csv          nvs + phy_init + factory(3MB)
sdkconfig.defaults      含 CONFIG_SOC_MIPI_DSI_SUPPORTED=y（关键，见下）
```

游戏核心（`game.h` / `render.h` / `actions.h` / `pixel_text.h` / `sfx.h`）
**直接引用上游仓库的文件，不复制**。改游戏代码请改 pixel-tower-defense 那边，
两边共用同一份源码。

---

## 与上游 Tab5 版的对应关系

| Tab5（Arduino + M5Unified） | Claw4（ESP-IDF + esp_lcd） |
|---|---|
| `M5.begin() / config() / setRotation(3)` | `g_bsp.init()`（DSI + 帧缓冲 + 背光） |
| `M5.Display`（M5GFX） | `GfxFb`（借用 BSP 的 PSRAM 帧缓冲） |
| `M5.update() + M5.Touch.getDetail()` | `touchRead()`（GT911 经 I2C） |
| `readTap()`（touch.h 内） | `tapRead()`（main.cpp 内，同一状态机） |
| `M5.Speaker.playRaw` + ES8388 寄存器 hack | `audioPlayRaw()`（I2S 标准模式直写） |
| `Preferences`（Arduino NVS） | `nvsInit/nvsGet*/nvsPut*`（ESP-IDF nvs_flash） |
| `M5.begin()` 的 PSRAM 检查 | `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` |

**Tab5 那120 行 ES8388 I2C 寄存器 hack 一行都没带过来**——那是 Tab5 专属的
PLL 不锁定修复，Claw4 的 codec 控制方式完全不同。

---

## 构建踩过的坑（都已在代码注释里标注）

1. **`CONFIG_SOC_MIPI_DSI_SUPPORTED=y` 必须显式打开。**
   IDF 把 `dsi/include` 的头路径挂在这个条件上，不开就找不到
   `esp_lcd_mipi_dsi.h`。症状极具迷惑性：报的是"所有 BSP 符号都未声明"
   （`GfxClaw4` / `touchRead` / `nvsPutU32` 全丢），因为 `bsp_claw4.h`
   整块被 `#if SOC_MIPI_DSI_SUPPORTED` 条件掉了。

2. **厂商头文件里的宏在 IDF 5.5 下编译不过。**
   `esp_lcd_nv3051f.h` 的三个初始化宏是按更新的 IDF 写的
   （`phy_clk_src = 0` 不能隐式转枚举、`.flags.use_dma2d` 字段不存在）。
   本移植把等价数值手写进 `bsp_claw4.cpp`（2 lane / 1000 Mbps / dpi 36MHz /
   官方 720×720 时序），**值与厂商宏完全一致**。

3. **IDF 5.5 vs 6.0 的 API 差异**（本机是 5.5.2，Claw4 要求 5.5.4）：
   - `esp_lcd_dsi_bus_config_t.lane_bit_rate_mbps`（6.0 起改名 `lane_rate_mbps`）
   - `esp_lcd_panel_disp_on_off`（不是 `esp_lcd_disp_on_off`）
   - `i2c_master_dev_handle_t`（不是 `i2c_device_handle_t`）
   - C++ 下裸 int 不能隐式转枚举（`gpio_num_t` / `i2s_port_t` / `ledc_channel_t`）

4. **Claw4 是双芯片设计**（ESP32-P4 主控 + ESP32-C5 协处理器 + NT26 4G）。
   本固件只用了 P4，未初始化 C5 与 4G 模块 —— 它们上电默认不干扰 P4 外设。

5. **音频是最大的未知数。** Claw4 官方固件用一个"专业蓝牙音频 codec"
   （型号未公开），本移植走 I2S 标准模式直写 PCM（BCLK=GPIO12, DOUT=GPIO9）。
   若真机无声，需要改成 `esp_codec_dev` 的 AudioCodec 接口
   （Claw4 固件有 `bt_audio_codec.h`）。

---

## 已知限制

- **窄屏强制英文**。720 宽下中文 16×16 字库（17px/字）放不进 116px 的
  建造栏槽位，见 `src/i18n.h` 的 `cnRenderable()`。用户在窄屏上选中文仍会被
  记住，回到宽屏依然生效。
- **PSRAM 必须 200MHz**。帧缓冲走 DSI DMA，时序敏感；`sdkconfig.defaults`
  里 `CONFIG_SPIRAM_SPEED_200M=y` 不要改。
- **模拟器仍有 23 项断言未过**（claw4 profile，268/291）。全是 UI 几何与数值
  配平，不影响游戏逻辑与固件构建。真机画面出来后再逐项调。

---

## 刷机步骤

```bash
cd ptd-claw4-fw
source ~/esp/esp-idf-v5.5/export.sh
idf.py -p /dev/tty.usbmodem10101 flash monitor
```

或者用合并镜像（一次烧全）：
```bash
esptool.py --chip esp32p4 -b 921600 \
  --before default_reset --after hard_reset \
  write_flash 0x0 build/ptd_claw4_merged.bin
```

启动日志应依次出现：
```
I (xxx) ptd: === PIXEL TOWER DEFENSE v0.5.0 | Claw4 | 720x720 ===
I (xxx) ptd: DSI LDO chan3 @2500mV acquired
I (xxx) ptd: DSI up: 720x720 RGB565 fb=0x3f...
I (xxx) ptd: audio init ok (I2S1 BCLK=12 DOUT=9 @16000Hz)
I (xxx) ptd: GT911 init done
I (xxx) ptd: unlock=0001 cleared=0000 levels=20 lang=EN
```
