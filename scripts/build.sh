#!/bin/sh
# 交叉编译 nas-server（在 Ubuntu 上运行，产物 scp 到开发板）。
#
# 用法:
#   ./scripts/build.sh [riscv64|aarch64|amd64] [额外的 cmake -D... 参数]
#   例: ./scripts/build.sh riscv64 -DURING_LIBRARY=/path/liburing.a
#
# 工具链前缀可通过 TC 覆盖（默认 ~/buildroot/output/host）。
# 采用静态链接（-static），产物无动态依赖。
set -e

ARCH="${1:-riscv64}"
if [ $# -gt 0 ]; then shift; fi
TC="${TC:-$HOME/buildroot/output/host}"

if [ ! -d "$TC/bin" ]; then
    echo "找不到工具链目录: $TC/bin" >&2
    echo "请设置 TC，例如: TC=/home/yv/buildroot/output/host $0" >&2
    exit 1
fi

echo "交叉编译 linux/$ARCH (TC=$TC, 静态链接) ..."
cmake -S . -B "build-$ARCH" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR="$ARCH" \
    -DCMAKE_C_COMPILER="$TC/bin/${ARCH}-buildroot-linux-gnu-gcc" \
    -DCMAKE_CXX_COMPILER="$TC/bin/${ARCH}-buildroot-linux-gnu-g++" \
    -DCMAKE_CXX_FLAGS="-O2" \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DNAS_BUILD_TESTS=OFF \
    "$@"

cmake --build "build-$ARCH" -j"$(nproc)"

echo "产物: build-$ARCH/nas-server"
echo "部署: scp build-$ARCH/nas-server root@192.168.137.200:/usr/bin/nas-server"
