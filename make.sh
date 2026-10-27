#!/usr/bin/env bash
# 交叉编译 Unicorn (x86_64 翻译用) 静态库 (arm64, iOS) -> unicorn/prebuilt/
# 用法: ./make.sh iOS
set -euo pipefail

TARGET="${1:-iOS}"
[ "$TARGET" = "iOS" ] || { echo "usage: ./make.sh iOS"; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/unicorn/prebuilt"
mkdir -p "$OUT/lib" "$OUT/include"

WORK="$(mktemp -d)"
echo "== clone unicorn-engine/unicorn =="
git clone --depth 1 https://github.com/unicorn-engine/unicorn.git "$WORK/unicorn"
cd "$WORK/unicorn"

IOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
DEPLOY="12.0"

mkdir -p build && cd build
cmake .. \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOY" \
  -DCMAKE_INSTALL_PREFIX="$OUT" \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DUNICORN_ARCH="x86_64" \
  -DUNICORN_BUILD_TESTS=OFF \
  -DUNICORN_BUILD_SAMPLES=OFF

cmake --build . -j"$(sysctl -n hw.ncpu)"
cmake --install .

# 兜底拷贝（install 路径因版本而异）
if [ ! -f "$OUT/lib/libunicorn.a" ]; then
  echo "libunicorn.a not at install path, searching..."
  f="$(find "$WORK/unicorn" -name 'libunicorn.a' | head -1)"
  [ -n "$f" ] && cp "$f" "$OUT/lib/"
fi
if [ ! -d "$OUT/include/unicorn" ]; then
  cp -R "$WORK/unicorn/include/unicorn" "$OUT/include/" 2>/dev/null || true
fi

echo "== Unicorn built =="
ls -la "$OUT/lib" "$OUT/include" 2>/dev/null || true
[ -f "$OUT/lib/libunicorn.a" ] || { echo "FATAL: libunicorn.a missing"; exit 1; }
[ -d "$OUT/include/unicorn" ] || { echo "FATAL: unicorn headers missing"; exit 1; }
