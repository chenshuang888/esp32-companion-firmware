# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# EXTRA_COMPONENT_DIRS：把项目内的自定义组件目录加入构建（本项目新增了 dynamic_app 组件用于 MicroQuickJS MVP 演示）。
set(EXTRA_COMPONENT_DIRS framework drivers app services storage dynamic_app)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(demo6)
