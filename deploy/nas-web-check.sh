#!/bin/sh
#
# nas-web-check.sh - 供镜像 watchdog 的 test-binary 调用的探活包装脚本。
#
# 放置：/usr/bin/nas-web-check.sh（chmod +x）
# 由 /etc/watchdog.conf 中的 test-binary 周期调用：
#     test-binary = /usr/bin/nas-web-check.sh
#     test-timeout = 15
#
# watchdog 的 test-binary 不支持传参，故用本脚本转发到 nas-server 的
# --health-check 子命令（对配置里的 http.host:http.port 探 /api/health）。
# 失败后重试，避免单次网络抖动直接把整块板子复位。
#
# 退出码：0 = 健康；非 0 = 不健康 -> watchdog 停止喂狗 -> 硬件复位。

BIN="${NAS_BIN:-/usr/bin/nas-server}"
CONF="${NAS_CONF:-/etc/nas/nas.conf}"
TRIES=3

i=1
while [ "$i" -le "$TRIES" ]; do
    if "$BIN" --health-check --config "$CONF" >/dev/null 2>&1; then
        exit 0
    fi
    i=$((i + 1))
    [ "$i" -le "$TRIES" ] && sleep 1
done

exit 1
