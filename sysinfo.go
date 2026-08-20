package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"strings"
	"syscall"
)

// SysInfo 系统基础信息
type SysInfo struct {
	Hostname    string  // 主机名
	OS          string  // 操作系统
	Arch        string  // 架构
	Kernel      string  // 内核版本
	CPUCores    int     // CPU 核心数
	CPUModel    string  // CPU 型号
	LoadAvg     string  // 负载
	Uptime      string  // 运行时间
	MemTotal    uint64  // 内存总量
	MemUsed     uint64  // 已用内存
	MemFree     uint64  // 空闲内存
	MemPercent  float64 // 内存使用率 %
	DiskTotal   uint64  // 磁盘总量
	DiskUsed    uint64  // 磁盘已用
	DiskFree    uint64  // 磁盘可用
	DiskPercent float64 // 磁盘使用率 %
}

// GetSysInfo 采集系统信息，nasPath 为需要统计磁盘的挂载点
func GetSysInfo(nasPath string) SysInfo {
	info := SysInfo{
		OS:       runtime.GOOS,
		Arch:     runtime.GOARCH,
		CPUCores: runtime.NumCPU(),
	}
	info.Hostname, _ = os.Hostname()
	info.CPUModel = readCPUModel()
	info.LoadAvg = readLoadAvg()
	info.Uptime = readUptime()
	info.Kernel = readKernel()

	info.MemTotal, info.MemFree = readMemInfo()
	info.MemUsed = info.MemTotal - info.MemFree
	if info.MemTotal > 0 {
		info.MemPercent = float64(info.MemUsed) / float64(info.MemTotal) * 100
	}

	info.DiskTotal, info.DiskFree = readDiskInfo(nasPath)
	info.DiskUsed = info.DiskTotal - info.DiskFree
	if info.DiskTotal > 0 {
		info.DiskPercent = float64(info.DiskUsed) / float64(info.DiskTotal) * 100
	}
	return info
}

// readMemInfo 从 /proc/meminfo 读取内存信息
func readMemInfo() (total, free uint64) {
	data, err := os.ReadFile("/proc/meminfo")
	if err != nil {
		return 0, 0
	}
	for _, line := range strings.Split(string(data), "\n") {
		fields := strings.Fields(line)
		if len(fields) < 2 {
			continue
		}
		val, err := strconv.ParseUint(fields[1], 10, 64)
		if err != nil {
			continue
		}
		// 单位为 kB，转换为字节
		switch fields[0] {
		case "MemTotal:":
			total = val * 1024
		case "MemFree:":
			free = val * 1024
		}
	}
	return
}

// readDiskInfo 使用 syscall.Statfs 获取挂载点磁盘空间
func readDiskInfo(path string) (total, free uint64) {
	var stat syscall.Statfs_t
	if err := syscall.Statfs(path, &stat); err != nil {
		return 0, 0
	}
	bsize := uint64(stat.Bsize)
	total = stat.Blocks * bsize
	free = stat.Bavail * bsize
	return
}

// readCPUModel 从 /proc/cpuinfo 读取 CPU 型号
func readCPUModel() string {
	data, err := os.ReadFile("/proc/cpuinfo")
	if err != nil {
		return "未知"
	}
	// 按优先级查找关键字，兼容 "key: value" 与 "key    : value" 两种格式
	for _, key := range []string{"model name", "Processor", "Hardware", "cpu model"} {
		for _, line := range strings.Split(string(data), "\n") {
			if !strings.HasPrefix(line, key) {
				continue
			}
			rest := line[len(key):]
			// 关键字后必须带冒号（中间可含空格），避免误匹配 "cpu MHz" 等
			if !strings.Contains(rest, ":") {
				continue
			}
			idx := strings.Index(rest, ":")
			v := strings.TrimSpace(rest[idx+1:])
			if v != "" {
				return v
			}
		}
	}
	return "未知"
}

// readLoadAvg 读取系统负载（1/5/15 分钟）
func readLoadAvg() string {
	data, err := os.ReadFile("/proc/loadavg")
	if err != nil {
		return "未知"
	}
	fields := strings.Fields(string(data))
	if len(fields) >= 3 {
		return fmt.Sprintf("%s / %s / %s", fields[0], fields[1], fields[2])
	}
	return "未知"
}

// readUptime 读取系统运行时间
func readUptime() string {
	data, err := os.ReadFile("/proc/uptime")
	if err != nil {
		return "未知"
	}
	fields := strings.Fields(string(data))
	if len(fields) == 0 {
		return "未知"
	}
	secs, err := strconv.ParseFloat(fields[0], 64)
	if err != nil {
		return "未知"
	}
	d := int(secs) / 86400
	h := (int(secs) % 86400) / 3600
	m := (int(secs) % 3600) / 60
	if d > 0 {
		return fmt.Sprintf("%d 天 %d 小时 %d 分", d, h, m)
	}
	if h > 0 {
		return fmt.Sprintf("%d 小时 %d 分", h, m)
	}
	return fmt.Sprintf("%d 分", m)
}

// readKernel 读取内核版本
func readKernel() string {
	if data, err := os.ReadFile("/proc/sys/kernel/osrelease"); err == nil {
		return strings.TrimSpace(string(data))
	}
	return runtime.Version()
}

// humanSize 将字节数转换为人类可读的容量字符串
func humanSize(n uint64) string {
	const unit = 1024
	if n < unit {
		return fmt.Sprintf("%d B", n)
	}
	div, exp := uint64(unit), 0
	for m := n / unit; m >= unit; m /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %ciB", float64(n)/float64(div), "KMGTPE"[exp])
}
