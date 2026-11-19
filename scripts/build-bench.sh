#!/bin/sh
# 交叉编译 RISC-V 目标上的 nas-server + io_bench + http_bench。
#
# 用法：
#   TC=~/buildroot/output/host sh scripts/build-bench.sh [riscv64] [--profile]
#   sh scripts/build-bench.sh --arch riscv64 --no-profile
#
# --profile（默认开启）会加 -g -fno-omit-frame-pointer，供板端 perf/火焰图使用；
# 不需要 perf 时用 --no-profile 减小体积。
set -eu

[ -f CMakeLists.txt ] || {
    echo "请在 NAS-Web-Cpp 项目根目录运行本脚本（当前目录没有 CMakeLists.txt）" >&2
    exit 1
}

ARCH="riscv64"
TC="${TC:-$HOME/buildroot/output/host}"
PROFILE=1

usage() {
    cat <<'EOF'
用法: build-bench.sh [arch] [选项]
  --arch ARCH       目标架构，默认 riscv64
  --tc PATH         Buildroot 工具链目录（默认 $HOME/buildroot/output/host）
  --profile         加 -g -fno-omit-frame-pointer（默认）
  --no-profile      只编译，不带调试符号
  -h, --help        显示帮助
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --arch)
            [ $# -ge 2 ] || { echo "缺少 --arch 参数" >&2; exit 1; }
            ARCH=$2
            shift 2
            ;;
        --tc)
            [ $# -ge 2 ] || { echo "缺少 --tc 参数" >&2; exit 1; }
            TC=$2
            shift 2
            ;;
        --profile)
            PROFILE=1
            shift
            ;;
        --no-profile)
            PROFILE=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "未知选项: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            ARCH=$1
            shift
            ;;
    esac
done

CC="$TC/bin/${ARCH}-buildroot-linux-gnu-gcc"
CXX="$TC/bin/${ARCH}-buildroot-linux-gnu-g++"
[ -x "$CC" ] || { echo "找不到 C 编译器: $CC" >&2; exit 1; }
[ -x "$CXX" ] || { echo "找不到 C++ 编译器: $CXX" >&2; exit 1; }

FLAGS="-O2"
if [ "$PROFILE" -eq 1 ]; then
    FLAGS="$FLAGS -g -fno-omit-frame-pointer"
fi

BUILD="build-$ARCH"
echo "== 交叉编译 $ARCH =="
echo "TC=$TC"
echo "FLAGS=$FLAGS"
echo "BUILD=$BUILD"

cmake -S . -B "$BUILD" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR="$ARCH" \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_CXX_FLAGS="$FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DNAS_BUILD_TESTS=OFF \
    -DNAS_BUILD_BENCH=ON \
    -DNAS_WITH_IO_URING=ON

cmake --build "$BUILD" -j"$(nproc)"

missing=0
for bin in nas-server io_bench http_bench; do
    if [ -x "$BUILD/$bin" ]; then
        echo "  OK  $BUILD/$bin"
    else
        echo "  MISSING  $BUILD/$bin" >&2
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "" >&2
    echo "缺少产物。io_bench 依赖 Buildroot sysroot 里的 liburing；" >&2
    echo "请确认 BR2_PACKAGE_LIBURING=y 且内核 CONFIG_IO_URING=y。" >&2
    exit 1
fi

cat <<EOF

下一步（板子 IP 按实际改）：
  scp $BUILD/nas-server $BUILD/io_bench $BUILD/http_bench \\
      root@192.168.137.200:/tmp/nas-bench/
  scp scripts/run-board-tests.sh root@192.168.137.200:/tmp/nas-bench/

或直接用一条命令：
  sh scripts/board-bench.sh --board root@192.168.137.200 --data-dir /media/sda1
EOF
