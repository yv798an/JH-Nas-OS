#!/bin/sh
# 没有 perf 时的火焰图采样/折叠工具（JH7110 厂商内核未编 CONFIG_PERF_EVENTS）。
#
# 板端采样（root；每次 attach 会短暂暂停进程，绝对性能数字会偏）：
#   sh flame-gdb.sh sample --pid <nas-server-pid> --seconds 30 \
#       --out flame-sync.gdb
#
# Ubuntu 折叠（再用 FlameGraph 的 flamegraph.pl 出 SVG）：
#   sh scripts/flame-gdb.sh fold flame-sync.gdb > flame-sync.folded
#   ~/FlameGraph/flamegraph.pl flame-sync.folded > flame-sync.svg
#
# 采样文件每块以 "### sample N" 开头，后面是 gdb 的 thread apply all bt；
# fold 把每个线程栈从 root 到 leaf 用 ';' 连起来并累加出现次数。
set -u

usage() {
    cat <<'EOF'
用法: flame-gdb.sh sample --pid PID --seconds N --out FILE [--interval S]
      flame-gdb.sh fold FILE
  --pid PID        被采样的进程 pid
  --seconds N      采样总时长，默认 30
  --out FILE       原始 gdb 输出文件
  --interval S     两次采样之间的等待秒数，默认 0
EOF
}

fold() {
    file=$1
    [ -f "$file" ] || { echo "文件不存在: $file" >&2; return 1; }
    awk '
        function flush(   i, s) {
            if (nfr == 0) return
            s = fr[nfr]
            for (i = nfr - 1; i >= 1; i--) s = s ";" fr[i]
            if (s != "") cnt[s]++
            nfr = 0
        }
        /^#[0-9]+[ \t]/ {
            line = $0
            sub(/^#[0-9]+[ \t]+/, "", line)
            sub(/^0x[0-9a-fA-F]+ in /, "", line)
            sub(/ at .*/, "", line)
            sub(/ from .*/, "", line)
            sub(/ \(\)$/, "", line)
            nfr++
            fr[nfr] = line
            next
        }
        { flush() }
        END {
            flush()
            for (s in cnt) print s, cnt[s]
        }
    ' "$file"
}

sample() {
    [ -n "$PID" ] || { echo "缺少 --pid" >&2; return 1; }
    [ -n "$OUT" ] || { echo "缺少 --out" >&2; return 1; }
    command -v gdb >/dev/null 2>&1 || { echo "找不到 gdb" >&2; return 1; }
    kill -0 "$PID" 2>/dev/null || { echo "进程不存在: $PID" >&2; return 1; }

    : > "$OUT" || return 1
    end=$(( $(date +%s) + SECONDS_TOTAL ))
    n=0
    while [ "$(date +%s)" -lt "$end" ]; do
        n=$((n + 1))
        printf '### sample %s\n' "$n" >> "$OUT"
        if command -v timeout >/dev/null 2>&1; then
            timeout 10 gdb -q -batch -p "$PID" \
                -ex 'set pagination off' \
                -ex 'set width 0' \
                -ex 'set height 0' \
                -ex 'set confirm off' \
                -ex 'set print frame-arguments none' \
                -ex 'thread apply all bt' >> "$OUT" 2>&1
        else
            gdb -q -batch -p "$PID" \
                -ex 'set pagination off' \
                -ex 'set width 0' \
                -ex 'set height 0' \
                -ex 'set confirm off' \
                -ex 'set print frame-arguments none' \
                -ex 'thread apply all bt' >> "$OUT" 2>&1
        fi
        [ "$INTERVAL" = "0" ] || sleep "$INTERVAL" 2>/dev/null || true
    done

    if [ -r "/proc/$PID/stat" ]; then
        st=$(awk '{print $3}' "/proc/$PID/stat" 2>/dev/null)
        [ "$st" = "T" ] && kill -CONT "$PID" 2>/dev/null
    fi

    if [ "$n" -eq 0 ]; then
        echo "没采到样本" >&2
        return 1
    fi
    threads=$(grep -c '^Thread ' "$OUT" 2>/dev/null || true)
    printf 'samples=%s thread_dumps=%s\n' "$n" "$threads" >&2
    if grep -q 'libthread_db' "$OUT" 2>/dev/null; then
        echo "WARN: gdb 找不到 libthread_db，可能只看到主线程" >&2
    fi
    return 0
}

cmd=${1:-}
case "$cmd" in
    sample|fold) shift ;;
    -h|--help|'') usage; exit 0 ;;
    *) echo "未知子命令: $cmd" >&2; usage >&2; exit 1 ;;
esac

if [ "$cmd" = "fold" ]; then
    [ $# -ge 1 ] || { echo "缺少输入文件" >&2; exit 1; }
    fold "$1"
    exit $?
fi

PID=""
SECONDS_TOTAL=30
OUT=""
INTERVAL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --pid)      PID=$2; shift 2 ;;
        --seconds)  SECONDS_TOTAL=$2; shift 2 ;;
        --out)      OUT=$2; shift 2 ;;
        --interval) INTERVAL=$2; shift 2 ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage >&2; exit 1 ;;
    esac
done

case "$SECONDS_TOTAL" in ''|*[!0-9]*) SECONDS_TOTAL=30 ;; esac
[ "$SECONDS_TOTAL" -gt 0 ] || SECONDS_TOTAL=30
case "$INTERVAL" in ''|*[!0-9.]*) INTERVAL=0 ;; esac

sample
