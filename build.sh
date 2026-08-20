#!/bin/sh
# NAS 文件浏览器交叉编译脚本
# 用法: ./build.sh [riscv64|arm64|amd64]
#   riscv64 = 星甲 JH7110 / VisionFive 2 等 RISC-V 芯片（默认）
#   arm64   = 大多数 ARM 开发板
#   amd64   = x86_64 PC
set -e

ARCH="${1:-riscv64}"

echo "🔨 交叉编译 linux/$ARCH ..."
CGO_ENABLED=0 GOOS=linux GOARCH="$ARCH" go build -trimpath -ldflags "-s -w" -o "nas-web-$ARCH" .
SIZE=$(du -h "nas-web-$ARCH" | cut -f1)
echo "✅ 生成 nas-web-$ARCH（${SIZE}，静态无依赖）"
echo "   部署到开发板: scp nas-web-$ARCH root@192.168.137.200:/usr/bin/nas-web"
