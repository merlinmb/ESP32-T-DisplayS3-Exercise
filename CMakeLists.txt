cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
add_compile_definitions(LV_CONF_PATH="${CMAKE_SOURCE_DIR}/include/lv_conf.h")

project(T_Display_S3_Strava_Exercise_Monitor)
