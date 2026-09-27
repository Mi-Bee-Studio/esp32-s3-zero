# For more information about build system see:
# https://docs.espressif.com/projects/espidf/latest/en/api-guides/build-system.html
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Git Bash（MSys）下 idf_tools.py 入口直接拒绝执行（MSYSTEM 由 MSYS 运行时
# 注入原生子进程，unset 无效），跳过 Python 依赖预检（同 env-station）。
idf_build_set_property(__CHECK_PYTHON 0)

project(blink)
