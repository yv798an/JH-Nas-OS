#!/bin/sh
# 板端 procfs 快照：按线程导出 CPU 时间与上下文切换。
#
# 用法（板端）：
#   sh proc-thread-stats.sh snapshot --pid <PID> --out /tmp/before.csv
#   ... 跑负载 ...
#   sh proc-thread-stats.sh snapshot --pid <PID> --out /tmp/after.csv
#
# 输出：
#   # pid=<pid> hz=<CLK_TCK> voluntary=<n> nonvoluntary=<n>
#   tid,comm,utime_ticks,stime_ticks,vcsw,nivcsw
#
# utime/stime 来自 /proc/<pid>/task/<tid>/stat（字段 14/15，单位 tick）；
# vcsw/nivcsw 来自 /proc/<pid>/task/<tid>/status。两次快照相减即可得到
# 该区间内每个线程的 CPU 时间与自愿/非自愿上下文切换增量。
set -u

usage() {
    cat <<'EOF'
用法: proc-thread-stats.sh snapshot --pid PID --out FILE
EOF
}

snapshot() {
    pid=$1
    out=$2
    [ -n "$pid" ] && [ -d "/proc/$pid" ] || {
        echo "进程不存在: $pid" >&2
        return 1
    }
    hz=$(getconf CLK_TCK 2>/dev/null || echo 100)
    pv=$(awk '/^voluntary_ctxt_switches/{print $2}' \
        "/proc/$pid/status" 2>/dev/null)
    pn=$(awk '/^nonvoluntary_ctxt_switches/{print $2}' \
        "/proc/$pid/status" 2>/dev/null)

    {
        printf '# pid=%s hz=%s voluntary=%s nonvoluntary=%s\n' \
            "$pid" "$hz" "${pv:-0}" "${pn:-0}"
        printf 'tid,comm,utime_ticks,stime_ticks,vcsw,nivcsw\n'
        for t in /proc/"$pid"/task/*; do
            [ -d "$t" ] || continue
            tid=${t##*/}
            comm=$(cat "$t/comm" 2>/dev/null | tr -d '\n' | tr ',' '_')
            line=$(cat "$t/stat" 2>/dev/null) || continue
            rest=${line##*) }
            ut=$(printf '%s\n' "$rest" | awk '{print $12}')
            st=$(printf '%s\n' "$rest" | awk '{print $13}')
            cv=$(awk '/^voluntary_ctxt_switches/{print $2}' \
                "$t/status" 2>/dev/null)
            cnv=$(awk '/^nonvoluntary_ctxt_switches/{print $2}' \
                "$t/status" 2>/dev/null)
            printf '%s,%s,%s,%s,%s,%s\n' \
                "$tid" "$comm" "${ut:-0}" "${st:-0}" "${cv:-0}" "${cnv:-0}"
        done
    } > "$out"
    echo "wrote $out"
}

cmd=${1:-}
[ $# -gt 0 ] && shift

case "$cmd" in
    snapshot)
        PID=""
        OUT=""
        while [ $# -gt 0 ]; do
            case "$1" in
                --pid) PID=$2; shift 2 ;;
                --out) OUT=$2; shift 2 ;;
                -h|--help) usage; exit 0 ;;
                *) echo "未知参数: $1" >&2; usage >&2; exit 1 ;;
            esac
        done
        [ -n "$PID" ] && [ -n "$OUT" ] || { usage >&2; exit 1; }
        snapshot "$PID" "$OUT"
        ;;
    -h|--help|'') usage ;;
    *) echo "未知子命令: $cmd" >&2; usage >&2; exit 1 ;;
esac
