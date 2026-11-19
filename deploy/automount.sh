#!/bin/sh
#
# automount.sh - mdev hotplug helper.
#   $1 = device name without /dev/ (e.g. sda1)
#   $2 = action (add/remove)
#
# 注意：BusyBox 的 blkid 不一定支持 `-o value -s TYPE`；这里直接解析
# `blkid DEV` 输出里的 TYPE="..."，避免把整行当成 fstype 传给 mount。
#
MNT_BASE="/media"
DEV_NAME=$1
ACTION=$2

if [ -z "$DEV_NAME" ]; then
    exit 1
fi

# mdev 可能对整盘（sda/sdb）也调用一次；只处理分区 sda1/sdb1。
case "$DEV_NAME" in
    sd[a-z][0-9]*) ;;
    *) exit 0 ;;
esac

DEV="/dev/${DEV_NAME}"
MNT_POINT="${MNT_BASE}/${DEV_NAME}"

log() {
    echo "automount: $*" > /dev/kmsg 2>/dev/null || echo "automount: $*"
}

is_mounted() {
    mountpoint -q "$1" 2>/dev/null || grep -qs " $1 " /proc/mounts
}

probe_fstype() {
    # BusyBox/full blkid 都走这里；TYPE="exfat"
    f=$(blkid "$DEV" 2>/dev/null | sed -n 's/.*TYPE="\([^"]*\)".*/\1/p')
    if [ -z "$f" ]; then
        f=$(blkid "$DEV" 2>/dev/null | sed -n 's/.*TYPE=\([^ ]*\).*/\1/p')
    fi
    if [ -z "$f" ]; then
        # 有些 BusyBox blkid 能读到 LABEL/UUID 但不输出 TYPE；
        # exFAT 的 magic 在偏移 3，内容是 "EXFAT"。
        magic=$(dd if="$DEV" bs=1 skip=3 count=5 2>/dev/null)
        [ "$magic" = "EXFAT" ] && f=exfat
    fi
    if [ -z "$f" ]; then
        # FAT32 的 magic 在偏移 82。
        magic=$(dd if="$DEV" bs=1 skip=82 count=5 2>/dev/null)
        [ "$magic" = "FAT32" ] && f=vfat
    fi
    if [ -z "$f" ]; then
        # 最后才用 -o value；某些 BusyBox 会返回整行，必须过滤掉。
        t=$(blkid -o value -s TYPE "$DEV" 2>/dev/null)
        case "$t" in
            ""|*[!A-Za-z0-9_]*) ;;
            *) f=$t ;;
        esac
    fi
    printf '%s' "$f"
}

mount_dev() {
    fstype=$(probe_fstype)
    log "add $DEV (fstype=${fstype:-unknown}) -> $MNT_POINT"

    case "$fstype" in
        vfat)
            mount -t vfat -o iocharset=utf8,umask=000 "$DEV" "$MNT_POINT" ||
                mount -t vfat -o umask=000 "$DEV" "$MNT_POINT" ||
                mount -t vfat "$DEV" "$MNT_POINT"
            ;;
        exfat)
            # 部分内核 exfat 不接受 iocharset/umask；逐级退回最小选项。
            mount -t exfat -o rw,umask=000,iocharset=utf8 "$DEV" "$MNT_POINT" ||
                mount -t exfat -o rw,umask=000 "$DEV" "$MNT_POINT" ||
                mount -t exfat "$DEV" "$MNT_POINT"
            ;;
        ntfs)
            if command -v ntfs-3g >/dev/null 2>&1; then
                ntfs-3g -o umask=000 "$DEV" "$MNT_POINT"
            else
                mount -t ntfs3 -o umask=000 "$DEV" "$MNT_POINT"
            fi
            ;;
        ext4|ext3|ext2)
            mount -t "$fstype" -o rw,noatime "$DEV" "$MNT_POINT"
            ;;
        "")
            mount "$DEV" "$MNT_POINT"
            ;;
        *)
            mount -t "$fstype" -o rw "$DEV" "$MNT_POINT"
            ;;
    esac
}

case "$ACTION" in
    add)
        [ -b "$DEV" ] || exit 0
        mkdir -p "$MNT_POINT"
        if is_mounted "$MNT_POINT"; then
            exit 0
        fi
        if mount_dev; then
            log "mounted $DEV on $MNT_POINT"
        else
            log "mount failed for $DEV"
            rmdir "$MNT_POINT" 2>/dev/null
            exit 1
        fi
        ;;
    remove)
        sync
        if is_mounted "$MNT_POINT"; then
            umount "$MNT_POINT" 2>/dev/null || umount -l "$MNT_POINT" 2>/dev/null
        fi
        rmdir "$MNT_POINT" 2>/dev/null
        ;;
esac

exit 0
