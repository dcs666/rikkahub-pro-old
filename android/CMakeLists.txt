# RikkaHub C 引擎 → Android .so
# 用法（NDK 交叉编译）：
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
#   cmake --build build
cmake_minimum_required(VERSION 3.22)
project(rikkahub_engine C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 引擎全部源文件（src/*/*.c，无 main，全部可进库）
file(GLOB_RECURSE RIKKA_SRC CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/../src/*/*.c")

add_library(rikka SHARED ${RIKKA_SRC})

target_include_directories(rikka PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")

# Android API 26+：系统自带 libssl/libcrypto/libz（bionic）
target_link_libraries(rikka PUBLIC ssl crypto z)

# 告警门禁与引擎一致（-Werror 构建期兜底）
target_compile_options(rikka PRIVATE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wformat=2 -Wundef -Wconversion -Wsign-conversion
    -Wwrite-strings -Wpointer-arith -Wcast-align)

set_target_properties(rikka PROPERTIES
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON)
