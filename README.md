# OrangePi NAS OS

基于 JH7110 RISC-V 架构、从零构建的轻量级嵌入式 NAS 系统镜像。

> 不使用现成发行版（Debian/Ubuntu），全部通过 Buildroot + 交叉编译工具链自行编译：**U-Boot → Linux Kernel → RootFS → 手动分区打包为可烧录镜像**。

---

## 硬件平台

| 项目 | 说明 |
|------|------|
| **开发板** | Orange Pi RV (VisionFive 2 兼容) |
| **SoC** | StarFive JH7110 (RISC-V 64, 四核) |
| **内存** | 2GB / 4GB / 8GB LPDDR4 |
| **存储介质** | TF 卡 (SD Card) |
| **网络** | 千兆以太网 (GMAC) |
| **外设** | USB 3.0 ×2, UART 调试串口, GPIO |

---

## 镜像编译后具备的能力

| 功能模块 | 说明 |
|----------|------|
| **基础系统** | BusyBox init 系统，Dropbear SSH 远程登录 |
| **网络** | 静态 IP 配置 / DHCP，mDNS 局域网发现 (`nas.local`) |
| **存储** | U 盘 / USB 硬盘热插拔自动挂载，支持 ext4 / vfat / NTFS / exFAT |
| **文件共享** | Samba (SMB/CIFS)，Windows 可直接访问 `\\nas` |
| **Web 管理** | Vue3 + Element Plus 前端，Go 单二进制后端 (`nasd`)，内嵌静态资源 |
| **安全** | 安全卸载 (sync + umount)，异常拔出自动清理 |
| **可靠性** | 硬件看门狗 (Watchdog)，OverlayFS 只读根文件系统防断电损坏 |
| **中文支持** | CP936/GB2312 编码，UTF-8，U 盘中文文件名不乱码 |

---

## 编译流程概览

整个镜像从源码到可烧录文件，需依次编译以下组件：

```
1. Buildroot 根文件系统
   └── 生成交叉编译工具链 + rootfs.tar

2. Linux Kernel (5.15-jh7110)
   └── 生成 Image + DTB (设备树)

3. U-Boot (2021.10-jh7110)
   └── 生成 u-boot-spl.bin + u-boot.itb

4. 手动打包镜像
   └── 创建分区表 → 写入 SPL/U-Boot/内核/根文件系统 → orange-pi-rv.img
```

---

## 一、编译 Buildroot (根文件系统)

### 1.1 进入配置界面

```bash
cd ~/buildroot
make menuconfig
```

### 1.2 关键配置项

| 配置菜单 | 必选项 |
|----------|--------|
| **Target options** | RISC-V 64, JH7110 |
| **Toolchain** | glibc, GCC 15, C++ support |
| **System configuration** | BusyBox init, `mdev` 设备管理, hostname=nas, root 密码 |
| **Target packages → Filesystem** | `ntfs-3g`, `dosfstools`, `exfat-utils`, `e2fsprogs` |
| **Target packages → Networking** | Dropbear SSH, Samba, Avahi |
| **Filesystem images** | `tar` (生成 rootfs.tar) |

> **注意**：不要勾选 `System configuration → Install timezone info`，存在 tzdata 编译 Bug（`zic: Can't open Asia/Shanghai`）。

### 1.3 创建 RootFS Overlay

在 `~/my_overlay/` 下准备自动挂载脚本和网络配置：

```
~/my_overlay/
├── etc/
│   ├── network/interfaces    # 静态 IP 配置
│   ├── mdev.conf             # 热插拔规则: sd[a-z][0-9]* 0:0 660 * /sbin/automount.sh
│   ├── profile               # 命令提示符 PS1 配置
│   └── init.d/
│       ├── S10mdev_daemon    # mdev -d 守护进程 (开机自启)
│       └── S99user_network   # DNS 持久化配置
└── sbin/
    └── automount.sh          # U 盘自动挂载脚本 (vfat/ntfs/ext4/exfat)
```

在 Buildroot 中关联 Overlay：

```
System configuration → Root FS overlay directories → /home/yv/my_overlay
```

### 1.4 编译

```bash
make -j$(nproc)
```

编译产物位于 `output/images/rootfs.tar`。

---

## 二、编译 Linux Kernel

### 2.1 准备交叉编译器

```bash
cd ~/OrangePi-RV/orange-pi-5.15-jh7110

export ARCH=riscv
export CROSS_COMPILE=/home/yv/buildroot/output/host/bin/riscv64-buildroot-linux-gnu-

# 验证编译器
${CROSS_COMPILE}gcc -v
```

### 2.2 内核配置

```bash
make starfive_jh7110_defconfig
make ARCH=riscv menuconfig
```

**必须启用的内核模块：**

| 类别 | 配置项 |
|------|--------|
| **网络** | `Packet socket`, `TCP/IP`, STMicroelectronics GMAC, Generic PHY |
| **USB** | `xHCI HCD`, `EHCI HCD`, `USB Mass Storage` |
| **文件系统** | `ext4`, `VFAT`, `exFAT`, `NTFS3` (内核驱动，非 FUSE) |
| **编码** | `CP936/GB2312`, `NLS UTF-8`, `Codepage 437` |

### 2.3 编译

```bash
# 关闭 GCC 插件 (GCC 15 不兼容 Linux 5.15 的插件)
sed -i 's/CONFIG_GCC_PLUGINS=y/# CONFIG_GCC_PLUGINS is not set/' .config
make ARCH=riscv olddefconfig

# 编译内核和设备树
make ARCH=riscv CROSS_COMPILE=/home/yv/buildroot/output/host/bin/riscv64-buildroot-linux-gnu- Image modules dtbs -j$(nproc)
```

### 2.4 验证

```bash
ls -lh arch/riscv/boot/Image
ls -lh arch/riscv/boot/dts/starfive/jh7110-orangepi-rv.dtb
```

---

## 三、编译 U-Boot

```bash
cd ~/OrangePi-RV/u-boot/v2021.10-jh7110

make ARCH=riscv starfive_visionfive2_defconfig

# GCC 15 兼容性修复
make ARCH=riscv \
  CROSS_COMPILE=/home/yv/buildroot/output/host/bin/riscv64-buildroot-linux-gnu- \
  KCFLAGS="-Wno-error=int-conversion" \
  OPENSBI_CFLAGS="-std=gnu11 -Wno-error" \
  -j$(nproc)
```

如果 `u-boot.itb` 未生成，手动补全：

```bash
# 复制缺失的 ITS 配置文件
cp uboot-fit-image.its jh7110-uboot-fit-image.its

# 重新打包生成 u-boot.itb
make ARCH=riscv CROSS_COMPILE=... KCFLAGS="..." OPENSBI_CFLAGS="..." u-boot.itb -j$(nproc)

# 验证
ls -lh u-boot.itb
```

---

## 四、手动打包 SD 卡镜像

### 4.1 SD 卡分区架构 (GPT)

JH7110 的 BootROM 对分区类型 GUID 有特定要求：

| 分区 | 大小 | 类型 GUID | 存放内容 |
|------|------|-----------|----------|
| **p1** | 2MB | `2E54B353-...` | `u-boot-spl.bin` |
| **p2** | 4MB | `5B0B4FE6-...` | `u-boot.itb` |
| **p3** | 200MB | FAT32 (`0700`) | `Image` + `jh7110-orangepi-rv.dtb` |
| **p4** | 剩余 | ext4 (`8300`) | 根文件系统 (rootfs.tar 解压) |

### 4.2 创建镜像并写入

```bash
cd ~/OrangePi-RV

# 1. 创建 2GB 空白镜像
dd if=/dev/zero of=orange-pi-rv.img bs=1M count=2000 status=progress

# 2. 分区 (gdisk 交互式操作，详见文档)
sudo gdisk orange-pi-rv.img

# 3. 映射为 Loop 设备
LOOP_DEV=$(sudo losetup -fP --show orange-pi-rv.img)

# 4. 写入 SPL 和 U-Boot
sudo dd if=u-boot/v2021.10-jh7110/spl/u-boot-spl.bin of=${LOOP_DEV}p1 bs=1M
sudo dd if=u-boot/v2021.10-jh7110/u-boot.itb of=${LOOP_DEV}p2 bs=1M

# 5. 格式化并挂载分区
sudo mkfs.vfat -F 32 -n "boot" ${LOOP_DEV}p3
sudo mkfs.ext4 -F -L "rootfs" ${LOOP_DEV}p4
mkdir -p /tmp/img_boot /tmp/img_rootfs
sudo mount ${LOOP_DEV}p3 /tmp/img_boot
sudo mount ${LOOP_DEV}p4 /tmp/img_rootfs

# 6. 拷贝内核与根文件系统
sudo cp orange-pi-5.15-jh7110/arch/riscv/boot/Image /tmp/img_boot/
sudo cp orange-pi-5.15-jh7110/arch/riscv/boot/dts/starfive/jh7110-orangepi-rv.dtb /tmp/img_boot/
sudo tar -xvf ~/buildroot/output/images/rootfs.tar -C /tmp/img_rootfs/

# 7. 刷盘并解除映射
sync
sudo umount /tmp/img_boot /tmp/img_rootfs
sudo losetup -d $LOOP_DEV
```

### 4.3 烧录到 SD 卡

使用 **balenaEtcher**（管理员模式）将 `orange-pi-rv.img` 写入 TF 卡。

---

## 五、首次启动

### 5.1 拨码开关

确保开发板拨码设置为 **SD 卡启动模式**（`BOOT0: 0, BOOT1: 0`）。

### 5.2 U-Boot 手动引导（仅首次）

如果启动时从 SPI Flash 引导而非 SD 卡，在 U-Boot 阶段执行：

```bash
mmc dev 1
fatload mmc 1:3 0x56000000 Image
fatload mmc 1:3 0x51000000 jh7110-orangepi-rv.dtb
setenv bootargs "console=ttyS0,115200 root=/dev/mmcblk1p4 rw rootwait earlycon"
booti 0x56000000 - 0x51000000
```

### 5.3 固化启动参数

进入系统后，保存 U-Boot 环境变量（下次自动启动）：

```bash
# 在 U-Boot 阶段 (开机按任意键进入 StarFive 提示符)
setenv bootcmd "mmc dev 1; fatload mmc 1:3 0x56000000 Image; fatload mmc 1:3 0x51000000 jh7110-orangepi-rv.dtb; booti 0x56000000 - 0x51000000"
setenv bootargs "console=ttyS0,115200 root=/dev/mmcblk1p4 rw rootwait earlycon"
saveenv
```

---

## 六、镜像增量更新

当只需替换内核或根文件系统时，无需重新制作完整镜像：

```bash
LOOP_DEV=$(sudo losetup -fP --show orange-pi-rv.img)
sudo mount ${LOOP_DEV}p3 /tmp/img_boot
sudo mount ${LOOP_DEV}p4 /tmp/img_rootfs

# 替换内核 / DTB
sudo cp orange-pi-5.15-jh7110/arch/riscv/boot/Image /tmp/img_boot/
sudo cp orange-pi-5.15-jh7110/arch/riscv/boot/dts/starfive/jh7110-orangepi-rv.dtb /tmp/img_boot/

# 替换根文件系统
sudo rm -rf /tmp/img_rootfs/*
sudo tar -xvf ~/buildroot/output/images/rootfs.tar -C /tmp/img_rootfs/

sync
sudo umount /tmp/img_boot /tmp/img_rootfs
sudo losetup -d $LOOP_DEV
```

如果直接操作 SD 卡，将 `${LOOP_DEV}p3` 替换为 `/dev/sdb3` 等实际设备名。

---

## 七、编译中遇到的已知问题及解决方案

| 问题 | 根因 | 解决方案 |
|------|------|----------|
| `zic: Can't open Asia/Shanghai` | Buildroot tzdata 编译 Bug | 关闭 `System configuration → Install timezone info` |
| `fatal error: gmp.h` | 缺少 GCC 插件依赖 | `sudo apt install -y libgmp-dev libmpc-dev libmpfr-dev` |
| GCC 插件语法错误 | GCC 15 与 Linux 5.15 内核插件不兼容 | `sed -i 's/CONFIG_GCC_PLUGINS=y/# CONFIG_GCC_PLUGINS is not set/' .config` |
| `error: 'bool' cannot be defined via 'typedef'` | C23 标准下 `bool` 成为关键字，与 OpenSBI 冲突 | `OPENSBI_CFLAGS="-std=gnu11 -Wno-error"` |
| `writeb/readl` int-conversion 错误 | GCC 15 严格类型检查 | `KCFLAGS="-Wno-error=int-conversion"` |
| `u-boot.itb` 未生成 | 缺少 `jh7110-uboot-fit-image.its` 文件 | `cp uboot-fit-image.its jh7110-uboot-fit-image.its` 后重新 make |
| U-Boot 从 SPI Flash 启动 | 拨码开关未切换到 SD 卡模式 | 调整硬件拨码，或手动 `fatload` + `booti` 引导 |
| USB 热插拔不触发挂载 | 新内核废弃 `/proc/sys/kernel/hotplug` | 改用 `mdev -d` 守护进程模式监听 Netlink |
| U 盘挂载后中文乱码 | 未加载中文字符集 | `mount -o iocharset=utf8` + 内核开启 CP936/NLS UTF-8 |

---

## 工作目录结构

```
~/OrangePi-RV/
├── buildroot/                      # Buildroot 构建工厂
│   └── output/images/rootfs.tar    #   编译产物
├── orange-pi-5.15-jh7110/          # Linux Kernel 源码
│   └── arch/riscv/boot/
│       ├── Image                   #   编译产物
│       └── dts/starfive/
│           └── jh7110-orangepi-rv.dtb
├── u-boot/v2021.10-jh7110/         # U-Boot 源码
│   ├── spl/u-boot-spl.bin          #   编译产物
│   └── u-boot.itb                  #   编译产物
├── my_overlay/                     # RootFS Overlay 配置
│   ├── etc/network/interfaces
│   ├── etc/mdev.conf
│   ├── etc/init.d/
│   └── sbin/automount.sh
└── orange-pi-rv.img                # 最终可烧录镜像
```

---

## 开发阶段

| 阶段 | 状态 | 内容 |
|------|------|------|
| **Phase 0** | 已完成 | Buildroot 编译、Kernel 启动、SSH 登录 |
| **Phase 1** | 已完成 | USB 识别、自动挂载/卸载、mdev 热插拔 |
| **Phase 2** | 进行中 | nasd 后台 (Go)、RESTful API、Samba 集成 |
| **Phase 3** | 规划中 | Vue3 Web 管理界面、mDNS 局域网发现 |
| **Phase 4** | 规划中 | OverlayFS 只读系统、Watchdog、OTA 升级 |

---

## 技术栈

- **构建系统**: Buildroot
- **交叉编译器**: riscv64-buildroot-linux-gnu (GCC 15)
- **Bootloader**: U-Boot 2021.10 + OpenSBI
- **内核**: Linux 5.15 (JH7110)
- **Init**: BusyBox init
- **设备管理**: mdev (守护进程模式)
- **SSH**: Dropbear
- **文件共享**: Samba
- **服务发现**: Avahi (mDNS)
