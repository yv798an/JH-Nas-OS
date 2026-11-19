#!/bin/sh
# Ubuntu 侧一键：交叉编译 -> scp 到板子 -> 板端跑完整测试 -> 拉回结果。
#
# 用法：
#   sh scripts/board-bench.sh --board root@192.168.137.200 \
#       --data-dir /media/sda1
#
# 默认使用 ~/buildroot/output/host 工具链，构建 build-riscv64。
# 板端使用 /tmp/nas-bench 下的测试二进制，不会覆盖 /usr/bin/nas-server。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
[ -f CMakeLists.txt ] || {
    echo "请在 NAS-Web-Cpp 项目根运行本脚本；$ROOT 下没有 CMakeLists.txt" >&2
    exit 1
}

BOARD="root@192.168.137.200"
DATA_DIR="/media/sda1"
ARCH="riscv64"
REMOTE_DIR="/tmp/nas-bench"
REMOTE_OUT="/tmp/nas-bench-out-$(date +%Y%m%d-%H%M%S)"
LOCAL_DIR="NAS_Test/data/run-$(date +%Y%m%d-%H%M%S)"
NO_BUILD=0
NO_PROFILE=0
EXTRA=""
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$HOME/FlameGraph}"

usage() {
    cat <<'EOF'
用法: board-bench.sh [选项]
  --board USER@HOST      板子 SSH 地址，默认 root@192.168.137.200
  --data-dir DIR         板端真实挂载点，默认 /media/sda1
  --arch ARCH            默认 riscv64
  --remote-dir DIR       板端临时目录，默认 /tmp/nas-bench
  --remote-out DIR       板端结果目录，默认 /tmp/nas-bench-out-<timestamp>
  --local-out DIR        本地结果目录，默认 NAS_Test/data/run-<timestamp>
  --no-build             跳过交叉编译，使用已有 build-<arch>
  --no-profile           不传 -g（没有 flamegraph 需求时）
  --size-mib N           透传给板端
  --repeat N             透传给板端
  --blocks LIST          透传给板端
  --depths LIST          透传给板端
  --http-threads LIST    透传给板端
  --http-rounds N        透传给板端
  --io-handoff MODE      yield|condvar：异步交接方式（默认 yield）
  --flame-seconds N      每个火焰图采样时长；无 perf 时自动回退 gdb 采样
  --skip-http            跳过 HTTP
  --skip-flame           跳过 perf record
  --skip-perf            跳过 perf stat
  --allow-any-fs         允许 tmpfs/rootfs（不推荐）
  -h, --help             显示帮助
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --board)         BOARD=$2; shift 2 ;;
        --data-dir)      DATA_DIR=$2; shift 2 ;;
        --arch)          ARCH=$2; shift 2 ;;
        --remote-dir)    REMOTE_DIR=$2; shift 2 ;;
        --remote-out)    REMOTE_OUT=$2; shift 2 ;;
        --local-out)     LOCAL_DIR=$2; shift 2 ;;
        --no-build)      NO_BUILD=1; shift ;;
        --no-profile)    NO_PROFILE=1; shift ;;
        --size-mib)      EXTRA="$EXTRA --size-mib $2"; shift 2 ;;
        --repeat)        EXTRA="$EXTRA --repeat $2"; shift 2 ;;
        --blocks)        EXTRA="$EXTRA --blocks $2"; shift 2 ;;
        --depths)        EXTRA="$EXTRA --depths $2"; shift 2 ;;
        --http-threads)  EXTRA="$EXTRA --http-threads $2"; shift 2 ;;
        --http-rounds)   EXTRA="$EXTRA --http-rounds $2"; shift 2 ;;
        --io-handoff)    EXTRA="$EXTRA --io-handoff $2"; shift 2 ;;
        --flame-seconds) EXTRA="$EXTRA --flame-seconds $2"; shift 2 ;;
        --skip-http)     EXTRA="$EXTRA --skip-http"; shift ;;
        --skip-flame)    EXTRA="$EXTRA --skip-flame"; shift ;;
        --skip-perf)     EXTRA="$EXTRA --skip-perf"; shift ;;
        --allow-any-fs)  EXTRA="$EXTRA --allow-any-fs"; shift ;;
        -h|--help)       usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage >&2; exit 1 ;;
    esac
done

BUILD="build-$ARCH"
command -v ssh >/dev/null 2>&1 || { echo "找不到 ssh" >&2; exit 1; }
command -v scp >/dev/null 2>&1 || { echo "找不到 scp" >&2; exit 1; }

if [ "$NO_BUILD" -eq 0 ]; then
    if [ "$NO_PROFILE" -eq 1 ]; then
        sh scripts/build-bench.sh --arch "$ARCH" --no-profile
    else
        sh scripts/build-bench.sh --arch "$ARCH" --profile
    fi
else
    echo "跳过构建，使用已有 $BUILD"
fi

for bin in nas-server io_bench http_bench; do
    [ -x "$BUILD/$bin" ] || { echo "缺少 $BUILD/$bin" >&2; exit 1; }
done

echo "== 部署到 $BOARD:$REMOTE_DIR =="
ssh "$BOARD" "mkdir -p '$REMOTE_DIR'"

scp "$BUILD/nas-server" "$BUILD/io_bench" "$BUILD/http_bench" \
    "$BOARD:$REMOTE_DIR/"
scp "scripts/run-board-tests.sh" "scripts/flame-gdb.sh" \
    "scripts/proc-thread-stats.sh" "$BOARD:$REMOTE_DIR/"

ssh "$BOARD" "chmod +x '$REMOTE_DIR/nas-server' '$REMOTE_DIR/io_bench' \
    '$REMOTE_DIR/http_bench' '$REMOTE_DIR/run-board-tests.sh' \
    '$REMOTE_DIR/flame-gdb.sh' '$REMOTE_DIR/proc-thread-stats.sh'"

echo "== 板端开始测试 =="
set +e
# shellcheck disable=SC2086
ssh "$BOARD" "'$REMOTE_DIR/run-board-tests.sh' \
    --bench-dir '$REMOTE_DIR' \
    --data-dir '$DATA_DIR' \
    --out '$REMOTE_OUT' $EXTRA"
RC=$?
set -e

echo "== 拉回结果到 $LOCAL_DIR =="
mkdir -p "$LOCAL_DIR"
set +e
# OpenSSH 9.x 的 scp 走 SFTP 时可能拒绝 "remote:/path/."，
# 优先用 tar 管道，兼容 BusyBox tar。
if ssh "$BOARD" "command -v tar >/dev/null 2>&1"; then
    ssh "$BOARD" "tar -C '$REMOTE_OUT' -cf - ." > "$LOCAL_DIR/.pull.tar"
    PULL_RC=$?
    if [ "$PULL_RC" -eq 0 ]; then
        tar -C "$LOCAL_DIR" -xf "$LOCAL_DIR/.pull.tar"
        PULL_RC=$?
    fi
    rm -f "$LOCAL_DIR/.pull.tar"
else
    scp -r "$BOARD:$REMOTE_OUT/." "$LOCAL_DIR/"
    PULL_RC=$?
fi
set -e
if [ "$PULL_RC" -ne 0 ]; then
    echo "WARN: 拉结果失败（remote=$REMOTE_OUT）；板端数据仍在，可手动 tar/scp" >&2
fi

for f in "$LOCAL_DIR"/flame-*.perf; do
    [ -f "$f" ] || continue
    if [ -d "$FLAMEGRAPH_DIR" ]; then
        "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" "$f" > "${f%.perf}.folded" 2>/dev/null || true
    fi
done

for f in "$LOCAL_DIR"/flame-*.gdb; do
    [ -f "$f" ] || continue
    sh scripts/flame-gdb.sh fold "$f" > "${f%.gdb}.folded" 2>/dev/null || true
done

if [ -d "$FLAMEGRAPH_DIR" ]; then
    for folded in "$LOCAL_DIR"/flame-*.folded; do
        [ -s "$folded" ] || continue
        "$FLAMEGRAPH_DIR/flamegraph.pl" "$folded" > "${folded%.folded}.svg" 2>/dev/null || true
    done
else
    echo "未找到 $FLAMEGRAPH_DIR；已生成 flame-*.folded，装好 FlameGraph 后再执行："
    echo "  ~/FlameGraph/flamegraph.pl $LOCAL_DIR/flame-sync.folded > flame-sync.svg"
fi

echo ""
echo "板端退出码: $RC"
echo "本地结果: $LOCAL_DIR"
exit "$RC"
