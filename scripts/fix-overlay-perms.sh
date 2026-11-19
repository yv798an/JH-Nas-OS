#!/bin/sh
# 在 Linux/WSL 里运行，给 rootfs overlay 的 init 脚本补执行位。
# Windows 上编辑/拷贝文件会丢 Unix 执行位，Buildroot 打包前跑一次即可。
set -e

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

chmod +x \
    my_overlay/etc/init.d/S* \
    my_overlay/sbin/automount.sh \
    my_overlay/usr/bin/nas-web-check.sh

echo "overlay permissions fixed under $ROOT/my_overlay"
