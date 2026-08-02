#!/usr/bin/env bash
# Build OpenSSL 3.0.13 for Android (arm64-v8a + x86_64) into app/src/main/jniLibs
#
# 背景: 引擎 librikka.so 链接 NDK 的 libssl/libcrypto stub(仅符号表, 无实现),
# 运行时依赖系统 libssl.so(BoringSSL)—— 部分设备(国产 ROM)不导出 SSL_new 等符号
# → UnsatisfiedLinkError: "SSL_new" referenced by librikka.so
# 解决: CI 里用 NDK 交叉编译真实 OpenSSL 3, 打进 APK jniLibs, 运行优先用 APK 内库。
# 注意: SONAME 必须为 libssl.so/libcrypto.so(无版本号), 否则 .so.3 文件不被 AGP 打包。
set -euo pipefail

VER=3.0.13
JNI="$(cd "$(dirname "$0")/.." && pwd)/app/src/main/jniLibs"
mkdir -p "$JNI/arm64-v8a" "$JNI/x86_64"

# ---- 1. 定位 NDK ----
if [ -n "${ANDROID_NDK_ROOT:-}" ]; then NDK="$ANDROID_NDK_ROOT"
elif [ -n "${ANDROID_NDK_HOME:-}" ]; then NDK="$ANDROID_NDK_HOME"
else NDK="$(ls -d "$ANDROID_HOME"/ndk/*/ 2>/dev/null | head -1 | sed 's/\/$//')"; fi
if [ -z "${NDK:-}" ] || [ ! -d "$NDK" ]; then
  echo "NDK not found, installing ndk;26.3.11579264 via sdkmanager..."
  SDKM="$(which sdkmanager 2>/dev/null || echo "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager")"
  yes | "$SDKM" --install "ndk;26.3.11579264" >/dev/null
  NDK="$(ls -d "$ANDROID_HOME"/ndk/*/ | head -1 | sed 's/\/$//')"
fi
echo "Using NDK: $NDK"
export ANDROID_NDK_ROOT="$NDK"
export PATH="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"

# ---- 2. 下载 OpenSSL 源码 ----
cd /tmp
if [ ! -d "openssl-$VER" ]; then
  curl -fsSL "https://github.com/openssl/openssl/archive/refs/tags/openssl-$VER.tar.gz" -o ossl.tgz
  tar xzf ossl.tgz
  # GitHub archive 解压目录名是 openssl-openssl-3.0.13(非 openssl-3.0.13)
  if [ ! -d "openssl-$VER" ]; then
    mv "openssl-openssl-$VER" "openssl-$VER"
  fi
fi
SRC="/tmp/openssl-$VER"

# ---- 3. 编译单个 ABI ----
build_abi() {
  local abi="$1" target="$2"
  local work="/tmp/ossl-$abi"
  echo "Building OpenSSL for $abi ($target)..."
  rm -rf "$work"; mkdir -p "$work"; cd "$work"
  perl "$SRC/Configure" "$target" -D__ANDROID_API__=26 shared no-tests --prefix="$work/out" > "/tmp/ossl-$abi-config.log" 2>&1 || {
    echo "Configure failed for $abi:"; tail -40 "/tmp/ossl-$abi-config.log"; exit 1; }
  # SONAME 去版本号: libssl.so / libcrypto.so (.so.3 不被 AGP 打包)
  sed -i 's/-Wl,-soname,libssl\.so\.3/-Wl,-soname,libssl.so/; s/-Wl,-soname,libcrypto\.so\.3/-Wl,-soname,libcrypto.so/' Makefile
  make -j"$(nproc)" build_libs > "/tmp/ossl-$abi.log" 2>&1 || { echo "make failed for $abi:"; tail -40 "/tmp/ossl-$abi.log"; exit 1; }
  cp -f libssl.so libcrypto.so "$JNI/$abi/"
  echo "  -> $JNI/$abi/: $(ls libssl.so libcrypto.so)"
}

build_abi arm64-v8a android-arm64
build_abi x86_64 android-x86_64
echo "OpenSSL $VER build done:"
ls -la "$JNI/arm64-v8a" "$JNI/x86_64"
