# Rebuilds libffmpegJNI.so exactly as it ships in Blueon: FFmpeg static libraries + ffmpeg_jni.cc.
#
# FFMPEG_DIR is the FFmpeg source tree after running build_ffmpeg.sh
# (it contains android-libs/<ABI>/libavcodec.a and the headers).
cmake_minimum_required(VERSION 3.22.1)
project(ffmpegJNI CXX)
set(CMAKE_CXX_STANDARD 11)

foreach(ffmpeg_lib avutil swresample swscale avcodec avformat)
    add_library(${ffmpeg_lib} STATIC IMPORTED)
    set_target_properties(${ffmpeg_lib} PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_DIR}/android-libs/${ANDROID_ABI}/lib${ffmpeg_lib}.a")
endforeach()

add_library(ffmpegJNI SHARED ffmpeg_jni.cc)
target_include_directories(ffmpegJNI PRIVATE "${FFMPEG_DIR}")

find_library(android_log_lib log)
# Order matters: users of a library come first.
target_link_libraries(ffmpegJNI
    PRIVATE android
    PRIVATE swresample
    PRIVATE swscale
    PRIVATE avformat
    PRIVATE avcodec
    PRIVATE avutil
    PRIVATE z
    PRIVATE ${android_log_lib})

if(ANDROID_ABI STREQUAL "arm64-v8a")
    target_link_options(ffmpegJNI PRIVATE "-Wl,-Bsymbolic")
endif()
target_link_options(ffmpegJNI PRIVATE "-Wl,-z,max-page-size=16384")
