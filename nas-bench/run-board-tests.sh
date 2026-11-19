#!/bin/sh
# 板端一键测试（RISC-V / Linux，需 root）。
#
# 覆盖：
#   - 环境与存储设备采集
#   - io_bench：块大小 × QD × 冷/热，CPU/延迟/上下文切换/缺页
#   - io_bench --verify：sync / io_uring 内容哈希一致性
#   - perf stat：代表性组合的 CPU/PMU 计数（有 perf 时）
#   - perf record / gdb 采样：sync / uring 火焰图数据（perf 不可用时回退到 gdb）
#   - http_bench：sync / uring 的端到端吞吐、客户端/服务端 CPU、延迟、哈希
#
# 用法（板上）：
#   /tmp/nas-bench/run-board-tests.sh --data-dir /media/sda1 \
#       --out /mnt/data/nas-bench-$(date +%Y%m%d-%H%M%S)
#
set -u

DATA_DIR="/media/sda1"
OUT=""
BENCH_DIR="/tmp/nas-bench"
SIZE_MIB=256
REPEAT=5
BLOCKS="4,8,16,32,64,128,256,512,1024"
DEPTHS="1,4,8,16,32,64"
HTTP_THREADS="1,2,4,8"
HTTP_ROUNDS=3
IO_HANDOFF="yield"
FLAME_SECONDS=20
SKIP_HTTP=0
SKIP_FLAME=0
SKIP_PERF=0
ALLOW_ANY_FS=0

NAS_WAS=0
WATCHDOG_WAS=0
SMB_WAS=0
AVAHI_WAS=0
TEST_SERVER_STARTED=0
LOAD_PID=""
IO_HASH=""
HAVE_PERF=0
HAVE_HW=0
HAVE_GDB=0
HZ=100

usage() {
    cat <<'EOF'
用法: run-board-tests.sh [选项]
  --data-dir DIR       测试文件所在挂载点，必须是真实挂载，默认 /media/sda1
  --out DIR            结果目录，默认 /mnt/data/nas-bench-<timestamp>
  --bench-dir DIR      二进制所在目录，默认 /tmp/nas-bench
  --size-mib N         测试文件大小，默认 256
  --repeat N           每个 IO 方案重复次数，默认 5
  --blocks LIST        块大小 KiB 列表，默认 4,8,16,32,64,128,256,512,1024
  --depths LIST        io_uring QD 列表，默认 1,4,8,16,32,64
  --http-threads LIST  HTTP 并发列表，默认 1,2,4,8
  --http-rounds N      每线程 HTTP 计时请求数，默认 3
  --io-handoff MODE    yield|condvar（异步交接方式），默认 yield
  --flame-seconds N    每个火焰图采样时长，默认 20
  --skip-http          跳过 HTTP 测试
  --skip-flame         跳过 perf record 火焰图
  --skip-perf          跳过 perf stat
  --allow-any-fs       允许在 tmpfs/rootfs 上测试（不推荐）
  -h, --help           显示帮助
EOF
}

log() {
    msg="[$(date '+%H:%M:%S')] $*"
    printf '%s\n' "$msg"
    printf '%s\n' "$msg" >> "${OUT:-/tmp}/run.log" 2>/dev/null || true
}

die() {
    log "ERROR: $*"
    exit 1
}

is_mounted() {
    mountpoint -q "$1" 2>/dev/null || grep -qs " $1 " /proc/mounts
}

fstype_of() {
    awk -v d="$1" '$2==d {print $3; exit}' /proc/mounts
}

pid_running() {
    if command -v pidof >/dev/null 2>&1; then
        pidof "$1" >/dev/null 2>&1
    else
        ps 2>/dev/null | grep -v grep | grep -q "$1"
    fi
}

cpu_ticks() {
    pid=$1
    [ -n "$pid" ] || { echo 0; return 0; }
    cat /proc/"$pid"/task/*/stat 2>/dev/null \
        | sed 's/^[0-9]* (.*) //' \
        | awk '{s += $12 + $13} END {print s + 0}'
}

uptime_s() {
    cut -d' ' -f1 /proc/uptime
}

cleanup() {
    [ -n "${LOAD_PID:-}" ] && kill "$LOAD_PID" 2>/dev/null
    if [ "${NAS_WAS:-0}" -eq 1 ] || [ "${TEST_SERVER_STARTED:-0}" -eq 1 ]; then
        [ -x /etc/init.d/S92nasweb ] && /etc/init.d/S92nasweb stop >/dev/null 2>&1
        TEST_SERVER_STARTED=0
        if [ "${NAS_WAS:-0}" -eq 1 ] && [ -x /etc/init.d/S92nasweb ]; then
            /etc/init.d/S92nasweb start >/dev/null 2>&1
        fi
    fi
    if [ "${WATCHDOG_WAS:-0}" -eq 1 ] && [ -x /etc/init.d/S95watchdog ]; then
        /etc/init.d/S95watchdog start >/dev/null 2>&1
    fi
    if [ "${SMB_WAS:-0}" -eq 1 ] && [ -x /etc/init.d/S91smb ]; then
        /etc/init.d/S91smb start >/dev/null 2>&1
    fi
    if [ "${AVAHI_WAS:-0}" -eq 1 ] && command -v avahi-daemon >/dev/null 2>&1; then
        avahi-daemon -D >/dev/null 2>&1
    fi
}

start_test_server() {
    conf=$1
    if NAS_BIN="$NAS_SERVER" NAS_CONF="$conf" /etc/init.d/S92nasweb start; then
        TEST_SERVER_STARTED=1
        return 0
    fi
    return 1
}

stop_test_server() {
    NAS_BIN="$NAS_SERVER" /etc/init.d/S92nasweb stop >/dev/null 2>&1
    TEST_SERVER_STARTED=0
}

make_conf() {
    value=$1
    out=$2
    if [ -f /etc/nas/nas.conf ]; then
        sed -e "s|^[[:space:]]*io\.uring[[:space:]]*=.*|io.uring = $value|" \
            -e "s|^[[:space:]]*io\.handoff[[:space:]]*=.*|io.handoff = $IO_HANDOFF|" \
            -e "s|^[[:space:]]*share\.roots[[:space:]]*=.*|share.roots = $TEST_ROOTS|" \
            /etc/nas/nas.conf > "$out"
    else
        : > "$out"
    fi
    grep -q '^[[:space:]]*io\.uring' "$out" || echo "io.uring = $value" >> "$out"
    grep -q '^[[:space:]]*io\.handoff' "$out" || echo "io.handoff = $IO_HANDOFF" >> "$out"
    grep -q '^[[:space:]]*share\.roots' "$out" || echo "share.roots = $TEST_ROOTS" >> "$out"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --data-dir)    DATA_DIR=$2; shift 2 ;;
        --out)         OUT=$2; shift 2 ;;
        --bench-dir)   BENCH_DIR=$2; shift 2 ;;
        --size-mib)    SIZE_MIB=$2; shift 2 ;;
        --repeat)      REPEAT=$2; shift 2 ;;
        --blocks)      BLOCKS=$2; shift 2 ;;
        --depths)      DEPTHS=$2; shift 2 ;;
        --http-threads) HTTP_THREADS=$2; shift 2 ;;
        --http-rounds) HTTP_ROUNDS=$2; shift 2 ;;
        --io-handoff)  IO_HANDOFF=$2; shift 2 ;;
        --flame-seconds) FLAME_SECONDS=$2; shift 2 ;;
        --skip-http)   SKIP_HTTP=1; shift ;;
        --skip-flame)  SKIP_FLAME=1; shift ;;
        --skip-perf)   SKIP_PERF=1; shift ;;
        --allow-any-fs) ALLOW_ANY_FS=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage >&2; exit 1 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || die "请用 root 运行"

case "$SIZE_MIB" in ''|*[!0-9]*) die "--size-mib 必须是整数" ;; esac
case "$REPEAT" in ''|*[!0-9]*) die "--repeat 必须是整数" ;; esac
case "$HTTP_ROUNDS" in ''|*[!0-9]*) die "--http-rounds 必须是整数" ;; esac
case "$FLAME_SECONDS" in ''|*[!0-9]*) die "--flame-seconds 必须是整数" ;; esac
case "$IO_HANDOFF" in yield|condvar) : ;; *) die "--io-handoff 必须是 yield|condvar" ;; esac
[ "$SIZE_MIB" -gt 0 ] || die "--size-mib 必须 > 0"

DATA_DIR=${DATA_DIR%/}
[ -n "$DATA_DIR" ] && [ "$DATA_DIR" != "/" ] || die "非法 --data-dir: $DATA_DIR"
[ -d "$DATA_DIR" ] || die "目录不存在: $DATA_DIR"
is_mounted "$DATA_DIR" || die "$DATA_DIR 不是挂载点（拒绝把 rootfs 当 U 盘）"

FSTYPE=$(fstype_of "$DATA_DIR")
case "$FSTYPE" in
    tmpfs|rootfs|overlay)
        [ "$ALLOW_ANY_FS" -eq 1 ] || die "$DATA_DIR 是 $FSTYPE，请换真实块设备挂载点"
        ;;
    ext4) : ;;
    *) log "WARN: $DATA_DIR 文件系统是 ${FSTYPE:-unknown}，ext4 最适合论文冷/热测" ;;
esac

if [ -z "$OUT" ]; then
    OUT="/mnt/data/nas-bench-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$OUT" || die "无法创建结果目录: $OUT"
LOG="$OUT/run.log"
: > "$LOG"
trap cleanup 0 1 2 15

AVAIL_KB=$(df -k "$DATA_DIR" 2>/dev/null | awk 'NR==2 {print $4}')
[ -n "$AVAIL_KB" ] || AVAIL_KB=0
NEED_KB=$((SIZE_MIB * 1024 + 65536))
[ "$AVAIL_KB" -ge "$NEED_KB" ] || die "$DATA_DIR 空间不足: avail=${AVAIL_KB}KiB need=${NEED_KB}KiB"

HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
case "$HZ" in ''|*[!0-9]*) HZ=100 ;; esac

IO_BENCH="$BENCH_DIR/io_bench"
[ -x "$IO_BENCH" ] || IO_BENCH="/usr/bin/io_bench"
HTTP_BENCH="$BENCH_DIR/http_bench"
[ -x "$HTTP_BENCH" ] || HTTP_BENCH="/usr/bin/http_bench"
NAS_SERVER="$BENCH_DIR/nas-server"
[ -x "$NAS_SERVER" ] || NAS_SERVER="/usr/bin/nas-server"
FLAME_GDB="$BENCH_DIR/flame-gdb.sh"
PROC_STATS="$BENCH_DIR/proc-thread-stats.sh"

[ -x "$IO_BENCH" ] || die "找不到 io_bench（$BENCH_DIR/io_bench 或 /usr/bin/io_bench）"
[ -x "$NAS_SERVER" ] || die "找不到 nas-server（$BENCH_DIR/nas-server 或 /usr/bin/nas-server）"
if [ "$SKIP_HTTP" -eq 0 ] || [ "$SKIP_FLAME" -eq 0 ]; then
    [ -x "$HTTP_BENCH" ] || die "找不到 http_bench（$BENCH_DIR/http_bench 或 /usr/bin/http_bench）"
fi
[ -x /etc/init.d/S92nasweb ] || die "缺少 /etc/init.d/S92nasweb"
grep -q 'NAS_BIN' /etc/init.d/S92nasweb || \
    die "S92nasweb 不支持 NAS_BIN，请先更新 overlay 里的 S92nasweb"
grep -q 'NAS_CONF' /etc/init.d/S92nasweb || \
    die "S92nasweb 不支持 NAS_CONF，请先更新 overlay 里的 S92nasweb"

# 记录并停止会干扰测试的服务；退出时恢复。
if [ -f /run/nas/nasweb.pid ]; then
    p=$(cat /run/nas/nasweb.pid 2>/dev/null)
    [ -n "$p" ] && kill -0 "$p" 2>/dev/null && NAS_WAS=1
fi
pid_running watchdog && WATCHDOG_WAS=1
{ pid_running smbd || pid_running nmbd; } && SMB_WAS=1
pid_running avahi-daemon && AVAHI_WAS=1

[ "$WATCHDOG_WAS" -eq 1 ] && [ -x /etc/init.d/S95watchdog ] && /etc/init.d/S95watchdog stop >/dev/null 2>&1
[ "$SMB_WAS" -eq 1 ] && [ -x /etc/init.d/S91smb ] && /etc/init.d/S91smb stop >/dev/null 2>&1
[ "$AVAHI_WAS" -eq 1 ] && killall avahi-daemon 2>/dev/null
[ "$NAS_WAS" -eq 1 ] && /etc/init.d/S92nasweb stop >/dev/null 2>&1

FILE="$DATA_DIR/nas_bench_${SIZE_MIB}MiB.bin"
WANT=$((SIZE_MIB * 1024 * 1024))
CUR=0
[ -f "$FILE" ] && CUR=$(stat -c %s "$FILE" 2>/dev/null || echo 0)
if [ "$CUR" != "$WANT" ]; then
    log "创建测试文件 $FILE ($SIZE_MIB MiB) ..."
    dd if=/dev/zero of="$FILE" bs=1M count="$SIZE_MIB" 2>/dev/null || die "dd 创建测试文件失败"
    sync
fi

ROOTS=$(sed -n 's/^[[:space:]]*share\.roots[[:space:]]*=[[:space:]]*//p' /etc/nas/nas.conf 2>/dev/null | tail -1)
in_roots=0
OLDIFS=$IFS
IFS=','
for r in $ROOTS; do
    [ -z "$r" ] && continue
    case "$DATA_DIR" in
        "$r"|"$r"/*) in_roots=1; break ;;
    esac
done
IFS=$OLDIFS
if [ "$in_roots" -eq 1 ]; then
    TEST_ROOTS="$ROOTS"
else
    if [ -n "$ROOTS" ]; then
        TEST_ROOTS="$ROOTS,$DATA_DIR"
    else
        TEST_ROOTS="$DATA_DIR"
    fi
fi
URL="http://127.0.0.1:8080/api/download?path=$FILE&download=1"

SYNC_CONF="$OUT/nas-sync.conf"
URING_CONF="$OUT/nas-uring.conf"
make_conf false "$SYNC_CONF"
make_conf true  "$URING_CONF"

if command -v perf >/dev/null 2>&1 && perf stat -e task-clock true >/dev/null 2>&1; then
    HAVE_PERF=1
fi
if [ "$HAVE_PERF" -eq 1 ] && perf stat -e cycles,instructions true >/dev/null 2>&1; then
    HAVE_HW=1
fi
if command -v gdb >/dev/null 2>&1; then
    HAVE_GDB=1
fi

collect_env() {
    {
        echo "date=$(date '+%Y-%m-%dT%H:%M:%S%z')"
        echo "uname=$(uname -a)"
        echo "cmdline=$(cat /proc/cmdline 2>/dev/null)"
        echo "data_dir=$DATA_DIR fstype=$FSTYPE"
        echo "file=$FILE"
        echo "size_mib=$SIZE_MIB repeat=$REPEAT"
        echo "blocks=$BLOCKS depths=$DEPTHS"
        echo "http_threads=$HTTP_THREADS http_rounds=$HTTP_ROUNDS"
        echo "io_handoff=$IO_HANDOFF"
        echo "have_perf=$HAVE_PERF have_hw=$HAVE_HW have_gdb=$HAVE_GDB HZ=$HZ"
        echo "--- cpuinfo ---"
        cat /proc/cpuinfo 2>/dev/null
        echo "--- mounts ---"
        cat /proc/mounts 2>/dev/null
        echo "--- df -k ---"
        df -k 2>/dev/null || true
        echo "--- lsblk ---"
        if command -v lsblk >/dev/null 2>&1; then
            lsblk -o NAME,ROTA,TYPE,FSTYPE,SIZE,MOUNTPOINT 2>/dev/null || true
        fi
        dev=$(awk -v d="$DATA_DIR" '$2==d {print $1; exit}' /proc/mounts)
        echo "device=$dev"
        case "$dev" in
            /dev/sd*) disk=$(basename "$dev" | sed 's/[0-9]*$//') ;;
            /dev/mmcblk*|/dev/nvme*|/dev/loop*) disk=$(basename "$dev" | sed 's/p[0-9]*$//') ;;
            *) disk="" ;;
        esac
        if [ -n "$disk" ]; then
            for f in scheduler read_ahead_kb rotational nr_requests; do
                p="/sys/block/$disk/queue/$f"
                [ -r "$p" ] && echo "$f=$(cat "$p")"
            done
        fi
        echo "--- cpufreq ---"
        for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor \
                 /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq; do
            [ -r "$f" ] && echo "$f=$(cat "$f")"
        done
        echo "--- thermal ---"
        for f in /sys/class/thermal/thermal_zone*/temp; do
            [ -r "$f" ] && echo "$f=$(cat "$f")"
        done
        echo "--- binaries ---"
        ls -l "$NAS_SERVER" "$IO_BENCH" "$HTTP_BENCH" 2>/dev/null || true
        echo "--- perf ---"
        if [ "$HAVE_PERF" -eq 1 ]; then
            perf list 2>/dev/null | sed -n '1,80p' || true
        else
            echo "perf not usable"
        fi
        echo "--- gdb ---"
        if [ "$HAVE_GDB" -eq 1 ]; then
            gdb --version 2>/dev/null | sed -n '1p' || true
        else
            echo "gdb not found"
        fi
    } > "$OUT/env.txt" 2>&1
}

run_io() {
    cache=$1
    warmup=$2
    log "IO $cache: blocks=$BLOCKS depths=$DEPTHS repeat=$REPEAT"
    "$IO_BENCH" --file "$FILE" --blocks "$BLOCKS" --depths "$DEPTHS" \
        --repeat "$REPEAT" --warmup "$warmup" --cache "$cache" --mode both \
        --handoff "$IO_HANDOFF" \
        --csv "$OUT/io-$cache.csv" > "$OUT/io-$cache.txt" 2>&1
    rc=$?
    cat "$OUT/io-$cache.txt" | tee -a "$LOG"
    [ "$rc" -eq 0 ] || log "WARN: io_bench $cache rc=$rc"
}

verify_io() {
    log "校验 sync/io_uring 内容哈希 ..."
    "$IO_BENCH" --file "$FILE" --block 64 --depths "1,8,64" --mode both \
        --handoff "$IO_HANDOFF" --verify > "$OUT/verify-io.txt" 2>&1
    rc=$?
    cat "$OUT/verify-io.txt" | tee -a "$LOG"
    IO_HASH=$(awk '
        $1=="VERIFY" {
            for (i=1; i<=NF; i++) {
                if ($i ~ /^hash=/) { sub(/^hash=/, "", $i); print $i; exit }
            }
        }' "$OUT/verify-io.txt")
    [ -n "$IO_HASH" ] || log "WARN: 没解析到 sync 哈希"
    [ "$rc" -eq 0 ] || log "WARN: verify rc=$rc（见 verify-io.txt）"
}

run_perf_stat() {
    [ "$HAVE_PERF" -eq 1 ] || return 0
    EVENTS="task-clock,context-switches,cpu-migrations,page-faults"
    if [ "$HAVE_HW" -eq 1 ]; then
        HW="cycles,instructions,branches,branch-misses,cache-references,cache-misses"
        if perf stat -e "$HW,$EVENTS" true >/dev/null 2>&1; then
            EVENTS="$HW,$EVENTS"
        fi
    fi
    log "perf stat: $EVENTS"
    {
        echo "perf_events=$EVENTS"
        for cache in cold hot; do
            for name in sync uring_qd1 uring_qd8 uring_qd64; do
                echo ""
                echo "### cache=$cache scheme=$name"
                set -- --file "$FILE" --block 64 --repeat 1 --cache "$cache" \
                    --handoff "$IO_HANDOFF"
                case "$name" in
                    sync)       set -- "$@" --mode sync ;;
                    uring_qd1)  set -- "$@" --mode uring --depth 1 ;;
                    uring_qd8)  set -- "$@" --mode uring --depth 8 ;;
                    uring_qd64) set -- "$@" --mode uring --depth 64 ;;
                esac
                perf stat -e "$EVENTS" -- "$IO_BENCH" "$@" 2>&1 || true
            done
        done
    } > "$OUT/perf-stat.txt" 2>&1
}

flame_one() {
    mode=$1
    conf=$2
    log "火焰图 $mode ..."
    if ! start_test_server "$conf"; then
        log "WARN: $mode server 启动失败，跳过火焰图"
        return 0
    fi
    pid=$(cat /run/nas/nasweb.pid 2>/dev/null)
    if [ -z "$pid" ]; then
        log "WARN: 没有 pid，跳过火焰图"
        stop_test_server
        return 0
    fi
    "$HTTP_BENCH" --url "$URL" --threads 4 --rounds 100000 \
        --label "flame-$mode" > "$OUT/flame-load-$mode.txt" 2>&1 &
    LOAD_PID=$!
    sleep 2
    profiled=0
    if [ "$HAVE_PERF" -eq 1 ]; then
        for ev in auto cpu-clock; do
            if [ "$ev" = "auto" ]; then
                perf record -F 499 -g -p "$pid" -o "$OUT/flame-$mode.data" \
                    -- sleep "$FLAME_SECONDS" > "$OUT/flame-$mode.record.txt" 2>&1 && profiled=1
            else
                perf record -e "$ev" -F 499 -g -p "$pid" -o "$OUT/flame-$mode.data" \
                    -- sleep "$FLAME_SECONDS" > "$OUT/flame-$mode.record.txt" 2>&1 && profiled=1
            fi
            [ "$profiled" -eq 1 ] && break
        done
        if [ "$profiled" -eq 1 ]; then
            perf script -i "$OUT/flame-$mode.data" > "$OUT/flame-$mode.perf" 2>/dev/null || true
            if command -v stackcollapse-perf.pl >/dev/null 2>&1 && \
               command -v flamegraph.pl >/dev/null 2>&1; then
                stackcollapse-perf.pl "$OUT/flame-$mode.perf" > "$OUT/flame-$mode.folded" 2>/dev/null || true
                flamegraph.pl "$OUT/flame-$mode.folded" > "$OUT/flame-$mode.svg" 2>/dev/null || true
            fi
        else
            log "WARN: perf record $mode 失败（见 flame-$mode.record.txt）"
        fi
    fi
    if [ "$profiled" -eq 0 ]; then
        if [ "$HAVE_GDB" -eq 1 ] && [ -x "$FLAME_GDB" ]; then
            log "回退 gdb 采样 $mode（${FLAME_SECONDS}s，采样时进程会被短暂暂停）"
            if "$FLAME_GDB" sample --pid "$pid" --seconds "$FLAME_SECONDS" \
                    --out "$OUT/flame-$mode.gdb" >> "$OUT/flame-$mode.record.txt" 2>&1; then
                log "gdb 采样完成: flame-$mode.gdb（拉到 Ubuntu 后用 flame-gdb.sh fold 出图）"
            else
                log "WARN: gdb 采样 $mode 失败（见 flame-$mode.record.txt）"
            fi
        elif [ "$HAVE_GDB" -eq 1 ]; then
            log "WARN: 缺少 $FLAME_GDB，跳过火焰图 $mode"
        else
            log "WARN: perf record 不可用且板端没有 gdb；跳过火焰图 $mode（装 gdb 或内核开 CONFIG_PERF_EVENTS）"
        fi
    fi
    kill "$LOAD_PID" 2>/dev/null
    wait "$LOAD_PID" 2>/dev/null
    LOAD_PID=""
    stop_test_server
}

proc_snapshot() {
    pid=$1
    out=$2
    [ -n "$pid" ] || return 0
    [ -x "$PROC_STATS" ] || return 0
    "$PROC_STATS" snapshot --pid "$pid" --out "$out" >/dev/null 2>&1 || true
}

run_http() {
    : > "$OUT/http-server-cpu.txt"
    echo "mode,threads,wall_s,server_cpu_pct" >> "$OUT/http-server-cpu.txt"

    for mode in sync uring; do
        if [ "$mode" = "sync" ]; then conf=$SYNC_CONF; else conf=$URING_CONF; fi
        log "HTTP $mode ...（client/server 同板，CPU 会互相竞争）"
        if ! start_test_server "$conf"; then
            log "WARN: $mode server 启动失败，跳过 HTTP"
            continue
        fi
        pid=$(cat /run/nas/nasweb.pid 2>/dev/null)

        "$HTTP_BENCH" --url "$URL" --threads 1 --rounds 1 --warmup 0 \
            --label "$mode-warm" > "$OUT/http-$mode-warm.txt" 2>&1 || true

        for t in $(echo "$HTTP_THREADS" | tr ',' ' '); do
            log "HTTP $mode threads=$t"
            s0=0; u0=""
            if [ -n "$pid" ]; then s0=$(cpu_ticks "$pid"); fi
            u0=$(uptime_s)
            proc_snapshot "$pid" "$OUT/proc-$mode-t$t.before.csv"
            "$HTTP_BENCH" --url "$URL" --threads "$t" --rounds "$HTTP_ROUNDS" \
                --warmup 0 --label "$mode-t$t" \
                --csv "$OUT/http-$mode-t$t.csv" > "$OUT/http-$mode-t$t.txt" 2>&1
            rc=$?
            proc_snapshot "$pid" "$OUT/proc-$mode-t$t.after.csv"
            cat "$OUT/http-$mode-t$t.txt" | tee -a "$LOG"
            u1=$(uptime_s)
            wall=$(awk -v a="$u0" -v b="$u1" 'BEGIN {printf "%.3f", b-a}')
            if [ -n "$pid" ]; then
                s1=$(cpu_ticks "$pid")
                pct=$(awk -v dt="$((s1 - s0))" -v hz="$HZ" -v w="$wall" \
                    'BEGIN {if (w > 0) printf "%.2f", (dt/hz)/w*100; else print "NA"}')
            else
                pct="NA"
            fi
            echo "$mode,$t,$wall,$pct" >> "$OUT/http-server-cpu.txt"
            log "  server_cpu_pct=$pct wall=$wall rc=$rc"
            [ "$rc" -eq 0 ] || log "WARN: http_bench $mode t$t rc=$rc"
        done

        if [ -n "$IO_HASH" ]; then
            "$HTTP_BENCH" --url "$URL" --threads 1 --rounds 1 --warmup 0 \
                --label "$mode-verify" --expect-hash "$IO_HASH" \
                --csv "$OUT/http-$mode-verify.csv" > "$OUT/http-$mode-verify.txt" 2>&1
            rc=$?
            cat "$OUT/http-$mode-verify.txt" | tee -a "$LOG"
            [ "$rc" -eq 0 ] || log "WARN: HTTP $mode 哈希校验 rc=$rc"
        else
            log "WARN: 没有 sync 哈希，跳过 HTTP 内容校验"
        fi
        stop_test_server
    done
}

log "OUT=$OUT"
log "data_dir=$DATA_DIR fstype=$FSTYPE file=$FILE"
collect_env
run_io cold 0
run_io hot 1
verify_io
if [ "$SKIP_PERF" -eq 0 ]; then
    run_perf_stat
else
    log "skip perf stat"
fi
if [ "$SKIP_FLAME" -eq 0 ] && { [ "$HAVE_PERF" -eq 1 ] || [ "$HAVE_GDB" -eq 1 ]; }; then
    flame_one sync "$SYNC_CONF"
    flame_one uring "$URING_CONF"
else
    log "skip flamegraph"
fi
if [ "$SKIP_HTTP" -eq 0 ]; then
    run_http
else
    log "skip HTTP"
fi

{
    echo "NAS bench summary"
    echo "out=$OUT"
    echo "data_dir=$DATA_DIR fstype=$FSTYPE file=$FILE"
    echo "size_mib=$SIZE_MIB repeat=$REPEAT"
    echo "blocks=$BLOCKS depths=$DEPTHS"
    echo "http_threads=$HTTP_THREADS http_rounds=$HTTP_ROUNDS"
    echo "io_handoff=$IO_HANDOFF"
    echo "have_perf=$HAVE_PERF have_hw=$HAVE_HW have_gdb=$HAVE_GDB"
    echo "io_hash=$IO_HASH"
    echo ""
    ls -l "$OUT"
} > "$OUT/summary.txt" 2>&1

log "完成。结果目录: $OUT"
