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

# JNI 桥（仅 Android/NDK 构建；本地 Linux 构建不含）
if(ANDROID)
    list(APPEND RIKKA_SRC
         "${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/cpp/engine_jni.c")
endif()

add_library(rikka SHARED ${RIKKA_SRC})

target_include_directories(rikka PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")

# 链接 NDK 的 libssl/libcrypto stub(NDK 提供符号表) → DT_NEEDED 写入
# libssl.so/libcrypto.so → 运行时 linker 优先加载 APK jniLibs 内的真 OpenSSL 3
# (由 android/scripts/build_openssl.sh 生成), 不再依赖系统 BoringSSL。
if(ANDROID)
    target_link_libraries(rikka PUBLIC z ssl crypto)
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,undefs")
else()
    target_link_libraries(rikka PUBLIC ssl crypto z)
endif()

# JNI 头（NDK 提供）
target_include_directories(rikka PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include")

# NDK 无 openssl 头：用 shim（系统 libssl.so 提供符号）
if(ANDROID)
    target_include_directories(rikka PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/ndk-include")
endif()

# 告警门禁与引擎一致（-Werror 构建期兜底）
target_compile_options(rikka PRIVATE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wformat=2 -Wundef -Wconversion -Wsign-conversion
    -Wwrite-strings -Wpointer-arith -Wcast-align)

set_target_properties(rikka PROPERTIES
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON)
