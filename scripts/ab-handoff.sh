#!/bin/sh
# A/B: yield vs condvar handoff (run on Ubuntu, board reachable over SSH).
#
# 实验内容（每个组合都跑）：
#   - sda1 / sdb1 的 64 KiB 冷读（QD 1,4,8,16,32,64，repeat=3）
#   - HTTP t1/2/4/8（sync 与 uring 服务端各 3 轮）
#   组合 = handoff(yield|condvar) × disk(sda1|sdb1)
#
# 用法：
#   sh scripts/ab-handoff.sh --board root@192.168.137.200
#   sh scripts/ab-handoff.sh --board root@192.168.137.200 --no-build
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

BOARD=${BOARD:-root@192.168.137.200}
SIZE_MIB=128
REPEAT=3
NO_BUILD=0

usage() {
    cat <<'EOF'
用法: ab-handoff.sh [选项]
  --board USER@HOST   板子 SSH 地址，默认 root@192.168.137.200
  --size-mib N        测试文件大小，默认 128
  --repeat N          每个方案重复次数，默认 3
  --no-build          跳过交叉编译（使用已有 build-riscv64）
  -h, --help          显示帮助
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --board)      BOARD=$2; shift 2 ;;
        --size-mib)   SIZE_MIB=$2; shift 2 ;;
        --repeat)     REPEAT=$2; shift 2 ;;
        --no-build)   NO_BUILD=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [ "$NO_BUILD" -eq 0 ]; then
    sh scripts/build-bench.sh --arch riscv64 --profile
fi
for bin in nas-server io_bench http_bench; do
    [ -x "build-riscv64/$bin" ] || { echo "缺少 build-riscv64/$bin" >&2; exit 1; }
done

for mode in yield condvar; do
    for disk in sda1 sdb1; do
        out="NAS_Test/data/ab-handoff-$mode-$disk"
        echo ""
        echo "===== handoff=$mode disk=$disk -> $out ====="
        sh scripts/board-bench.sh \
            --board "$BOARD" \
            --data-dir "/media/$disk" \
            --local-out "$out" \
            --no-build \
            --size-mib "$SIZE_MIB" \
            --repeat "$REPEAT" \
            --blocks 64 \
            --depths 1,4,8,16,32,64 \
            --http-threads 1,2,4,8 \
            --http-rounds 3 \
            --io-handoff "$mode" \
            --skip-flame \
            --skip-perf
    done
done

echo ""
echo "===== 汇总 ====="
PY=python3
command -v python3 >/dev/null 2>&1 || PY=python
"$PY" scripts/compare-handoff.py
# 机制分解用 yield 基线（展示原始交接开销）；优化前后对比看 summary。
"$PY" scripts/analyze-decomp.py --mode yield
"$PY" scripts/plot-decomp.py
