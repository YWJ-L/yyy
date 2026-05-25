cmake_minimum_required(VERSION 3.22.1)

project(wifiufo)

set(NATIVE_APP_GLUE_DIR ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(
    native_app_glue
    STATIC
    ${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c
)
target_include_directories(native_app_glue PUBLIC ${NATIVE_APP_GLUE_DIR})

add_library(
    wifiufo
    SHARED
    native-lib.cpp
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles-lib GLESv2)

target_link_libraries(
    wifiufo
    native_app_glue
    ${android-lib}
    ${egl-lib}
    ${gles-lib}
    ${log-lib}
)
