# Rebuilds libmp3lame.so exactly as it ships in Blueon: unmodified LAME 3.100 + config.h from this folder.
#
# LAME_DIR is the unpacked lame-3.100 source tree. This config.h replaces the one configure would write.
cmake_minimum_required(VERSION 3.22.1)
project(mp3lame C)

file(GLOB LAME_SOURCES ${LAME_DIR}/libmp3lame/*.c)
add_library(mp3lame SHARED ${LAME_SOURCES})
target_include_directories(mp3lame PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LAME_DIR}/include
    ${LAME_DIR}/libmp3lame)
target_compile_definitions(mp3lame PRIVATE HAVE_CONFIG_H)
target_compile_options(mp3lame PRIVATE -w)
target_link_options(mp3lame PRIVATE "-Wl,-z,max-page-size=16384")
