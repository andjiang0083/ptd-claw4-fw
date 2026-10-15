# main/CMakeLists.txt — PTD Claw4 固件
#
# 包含两处外部源码：
#   1. BSP（本目录）—— DSI / GT911 / I2S / NVS，Claw4 专属
#   2. 游戏核心（上游仓库 src/）—— 与 PC 模拟器共用的同一份 header
#
# ⚠️ 生成物 config_gen.h 不在上游 src/ 里（v0.5.0 起移到 build/<profile>/），
#    由 tools/layout.py --profile claw4 生成到 PTD_GEN_DIR。
#    这里的 include 顺序保证先命中 PTD_GEN_DIR，不会误用 tab5 的那份。

idf_component_register(
    SRCS
        "bsp_claw4.cpp"
        # Claw4 官方 NV3051F 驱动（含厂商 init command 序列，勿改——
        # 这张 720x720 MIPI-DSI 面板能点亮全靠它）
        "esp_lcd_nv3051f.c"
        "main.cpp"
    INCLUDE_DIRS
        "."
        "${PTD_GEN_DIR}"
        "${PTD_SRC_ROOT}/src"
    REQUIRES
        esp_lcd
        esp_hw_support
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_gpio
        nvs_flash
        esp_timer
)

# 平台宏：告诉 src/sfx.h 走 Claw4 音频后端
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PTD_AUDIO_CLAW4=1
)

# ⚠️ 上游代码风格是 2 空格缩进 + 无花括号单行 if，clang 的
#    -Wmisleading-indentation 会把"视觉上像兄弟、实际是嵌套"的情况报出来，
#    而 espressif 的默认构建带 -Werror → 直接编译失败。
#    这不是我们引入的问题，是上游风格与 clang 严格模式的冲突。
#    游戏逻辑一行不改，只在本组件关掉这个检查。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-misleading-indentation
    -Wno-error=misleading-indentation
)
