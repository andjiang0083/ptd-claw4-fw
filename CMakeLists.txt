# PIXEL TOWER DEFENSE — Metalio Claw4 固件
#
# 关键决策：游戏源码（game.h / render.h / actions.h / pixel_text.h / sfx.h）
# 直接引用上游仓库的文件，**不复制**。单��真源在sim/ 与真机之间保持一致。
cmake_minimum_required(VERSION 3.16)

# 目标板：ESP32-P4（Claw4 主控）
set(SUPPORTED_TARGETS esp32p4)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 上游仓库路径（可用 -DPTD_SRC_ROOT= 覆盖）
set(PTD_SRC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../pixel-tower-defense"
    CACHE PATH "pixel-tower-defense 仓库根目录")
set(PTD_GEN_DIR  "${CMAKE_CURRENT_LIST_DIR}/gen"
    CACHE PATH "生成的 config_gen.h 所在目录")

project(ptd_claw4)
