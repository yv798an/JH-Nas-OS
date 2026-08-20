package main

import (
	"embed"
	"errors"
	"fmt"
	"html/template"
	"io/fs"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

//go:embed templates/*.html
var templateFS embed.FS

//go:embed static/*
var staticFS embed.FS

var staticSubFS, _ = fs.Sub(staticFS, "static")

var (
	nasRoots  []NASRoot
	tmplIndex *template.Template
	tmplView  *template.Template
)

// NASRoot 一个存储根（如一个 U 盘挂载点）
type NASRoot struct {
	Name string // 显示名（路径 basename，如 sda1）
	Path string // 实际挂载路径（如 /media/sda1）
}

// FileEntry 目录列表中的一条记录
type FileEntry struct {
	Name    string // 文件名
	RelPath string // 相对 NAS 根目录的路径（用于链接）
	IsDir   bool
	Size    string
	ModTime string
}

// Breadcrumb 面包屑导航项
type Breadcrumb struct {
	Name string
	Path string // 相对路径，空表示根
}

// IndexData 首页（文件浏览器 + 系统信息）数据
type IndexData struct {
	Sys        SysInfo
	RootsLabel string // 所有存储名，如 "sda1, sdb1"
	RootName   string // 当前浏览的存储名（顶层为 "NAS 存储"）
	CurrentRel string
	Breadcrumb []Breadcrumb
	ParentRel  string
	Entries    []FileEntry
	Empty      bool
	Error      string
}

// ViewData 文件查看页数据
type ViewData struct {
	FileName string
	RelPath  string
	DirRel   string
	Size     string
	ModTime  string
	IsText   bool
	Content  string
}

// loadTemplates 加载内嵌的 HTML 模板
func loadTemplates() error {
	funcs := template.FuncMap{
		"humanSize": humanSize,
	}
	var err error
	tmplIndex, err = template.New("index.html").Funcs(funcs).ParseFS(templateFS, "templates/index.html")
	if err != nil {
		return err
	}
	tmplView, err = template.New("view.html").Funcs(funcs).ParseFS(templateFS, "templates/view.html")
	if err != nil {
		return err
	}
	return nil
}

// parseRoots 解析存储根列表。-root 支持逗号分隔多个挂载点；
// 为空时自动探测常用位置（~/nas_store、/media/sda1、/media/sdb1）。
func parseRoots(flagVal string) []NASRoot {
	var paths []string
	if flagVal != "" {
		for _, p := range strings.Split(flagVal, ",") {
			if p = strings.TrimSpace(p); p != "" {
				paths = append(paths, p)
			}
		}
	} else {
		home, _ := os.UserHomeDir()
		candidates := []string{
			filepath.Join(home, "nas_store"),
			"/media/sda1",
			"/media/sdb1",
		}
		for _, c := range candidates {
			if info, err := os.Stat(c); err == nil && info.IsDir() {
				paths = append(paths, c)
			}
		}
	}

	seen := map[string]bool{}
	var roots []NASRoot
	for _, p := range paths {
		abs, _ := filepath.Abs(p)
		if abs == "" || seen[abs] {
			continue
		}
		seen[abs] = true
		if info, err := os.Stat(abs); err != nil || !info.IsDir() {
			log.Printf("⚠️ 跳过无效存储路径: %s", p)
			continue
		}
		name := filepath.Base(abs)
		// 处理同名冲突（如同时挂载两个 sda1）
		for _, r := range roots {
			if r.Name == name {
				name = name + "_" + strconv.Itoa(len(roots)+1)
			}
		}
		roots = append(roots, NASRoot{Name: name, Path: abs})
	}
	return roots
}

// errTopLevel 表示请求的是存储根列表（顶层）页面
var errTopLevel = errors.New("顶层")

// resolvePath 将用户传入的相对路径解析为绝对路径，并返回所属存储根。
// 相对路径第一段为存储名（如 "sda1"），其后为该存储内的路径，防止路径穿越（../）逃出挂载点。
func resolvePath(rel string) (full string, root NASRoot, err error) {
	rel = strings.TrimSpace(rel)
	rel = strings.ReplaceAll(rel, "\\", "/")
	rel = strings.TrimPrefix(rel, "/")
	if rel == "" {
		return "", NASRoot{}, errTopLevel
	}

	segs := strings.Split(rel, "/")
	name := segs[0]

	// 查找对应的存储根
	found := false
	for _, r := range nasRoots {
		if r.Name == name {
			root = r
			found = true
			break
		}
	}
	if !found {
		return "", NASRoot{}, errors.New("未知存储: " + name)
	}

	rest := strings.Join(segs[1:], "/")
	full = filepath.Join(root.Path, filepath.FromSlash(rest))

	// 越界防护：完整路径必须位于该存储根之内
	rootAbs, err := filepath.Abs(root.Path)
	if err != nil {
		return "", NASRoot{}, err
	}
	fullAbs, err := filepath.Abs(full)
	if err != nil {
		return "", NASRoot{}, err
	}
	if fullAbs != rootAbs && !strings.HasPrefix(fullAbs, rootAbs+string(filepath.Separator)) {
		return "", NASRoot{}, errors.New("路径越界: " + rel)
	}
	return fullAbs, root, nil
}

// handleIndex 文件浏览器主页（支持 ?dir= 进入子目录，顶层显示所有存储根）
func handleIndex(w http.ResponseWriter, r *http.Request) {
	// 只处理 / 根路径，其余交给其它 handler
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}

	dirRel := r.URL.Query().Get("dir")

	// 顶层：显示所有存储根（U 盘挂载点）
	if strings.Trim(dirRel, "/") == "" {
		renderTopLevel(w)
		return
	}

	dirPath, root, err := resolvePath(dirRel)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	info, err := os.Stat(dirPath)
	if err != nil {
		http.Error(w, "目录不存在", http.StatusNotFound)
		return
	}
	if !info.IsDir() {
		http.Error(w, "不是目录", http.StatusBadRequest)
		return
	}

	entries, err := os.ReadDir(dirPath)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	data := IndexData{
		Sys:        GetSysInfo(root.Path),
		RootsLabel: rootsLabel(),
		RootName:   root.Name,
		CurrentRel: filepath.ToSlash(dirRel),
		Breadcrumb: buildBreadcrumb(dirRel),
		ParentRel:  parentRel(dirRel),
	}

	for _, e := range entries {
		info, _ := e.Info()
		rel := filepath.ToSlash(filepath.Join(dirRel, e.Name()))
		fe := FileEntry{
			Name:    e.Name(),
			RelPath: rel,
			IsDir:   e.IsDir(),
			ModTime: info.ModTime().Format("2006-01-02 15:04:05"),
		}
		if e.IsDir() {
			fe.Size = "目录"
		} else {
			fe.Size = humanSize(uint64(info.Size()))
		}
		data.Entries = append(data.Entries, fe)
	}

	// 目录排在前面，同类型按名称排序
	sort.Slice(data.Entries, func(i, j int) bool {
		if data.Entries[i].IsDir != data.Entries[j].IsDir {
			return data.Entries[i].IsDir
		}
		return strings.ToLower(data.Entries[i].Name) < strings.ToLower(data.Entries[j].Name)
	})
	data.Empty = len(data.Entries) == 0

	if err := tmplIndex.Execute(w, data); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
	}
}

// renderTopLevel 渲染存储根列表（顶层页面）
func renderTopLevel(w http.ResponseWriter) {
	if len(nasRoots) == 0 {
		http.Error(w, "未配置任何存储根", http.StatusInternalServerError)
		return
	}

	data := IndexData{
		Sys:        GetSysInfo(nasRoots[0].Path),
		RootsLabel: rootsLabel(),
		RootName:   "NAS 存储",
		Breadcrumb: []Breadcrumb{{Name: "NAS 存储根目录", Path: ""}},
	}

	for _, r := range nasRoots {
		total, free := readDiskInfo(r.Path)
		fe := FileEntry{
			Name:    r.Name,
			RelPath: r.Name,
			IsDir:   true,
			ModTime: "-",
		}
		if total > 0 {
			used := total - free
			fe.Size = fmt.Sprintf("%s / %s", humanSize(used), humanSize(total))
		} else {
			fe.Size = "存储"
		}
		data.Entries = append(data.Entries, fe)
	}
	data.Empty = len(data.Entries) == 0

	if err := tmplIndex.Execute(w, data); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
	}
}

// rootsLabel 返回所有存储名拼接字符串，如 "sda1, sdb1"
func rootsLabel() string {
	names := make([]string, 0, len(nasRoots))
	for _, r := range nasRoots {
		names = append(names, r.Name)
	}
	return strings.Join(names, ", ")
}

// handleView 查看文件内容（文本渲染 / 非文本提示）
func handleView(w http.ResponseWriter, r *http.Request) {
	rel := r.URL.Query().Get("path")
	if rel == "" {
		http.Error(w, "缺少文件路径", http.StatusBadRequest)
		return
	}
	full, _, err := resolvePath(rel)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	info, err := os.Stat(full)
	if err != nil {
		http.Error(w, "文件不存在", http.StatusNotFound)
		return
	}
	if info.IsDir() {
		http.Redirect(w, r, "/?dir="+url.QueryEscape(rel), http.StatusFound)
		return
	}

	data := ViewData{
		FileName: filepath.Base(full),
		RelPath:  rel,
		DirRel:   parentRel(rel),
		Size:     humanSize(uint64(info.Size())),
		ModTime:  info.ModTime().Format("2006-01-02 15:04:05"),
		IsText:   isTextFile(full),
	}

	if data.IsText {
		content, err := os.ReadFile(full)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		data.Content = string(content)
	}

	if err := tmplView.Execute(w, data); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
	}
}

// handleRaw 在线预览：按 MIME 类型直接返回文件内容（图片等）
func handleRaw(w http.ResponseWriter, r *http.Request) {
	rel := r.URL.Query().Get("path")
	if rel == "" {
		http.Error(w, "缺少文件路径", http.StatusBadRequest)
		return
	}
	full, _, err := resolvePath(rel)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	info, err := os.Stat(full)
	if err != nil || info.IsDir() {
		http.Error(w, "文件不存在", http.StatusNotFound)
		return
	}
	http.ServeFile(w, r, full)
}

// handleDownload 下载文件（以附件形式）
func handleDownload(w http.ResponseWriter, r *http.Request) {
	rel := r.URL.Query().Get("path")
	if rel == "" {
		http.Error(w, "缺少文件路径", http.StatusBadRequest)
		return
	}
	full, _, err := resolvePath(rel)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	info, err := os.Stat(full)
	if err != nil || info.IsDir() {
		http.Error(w, "文件不存在", http.StatusNotFound)
		return
	}
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", filepath.Base(full)))
	http.ServeFile(w, r, full)
}

// buildBreadcrumb 根据相对路径构建面包屑
func buildBreadcrumb(rel string) []Breadcrumb {
	bc := []Breadcrumb{{Name: "NAS 根目录", Path: ""}}
	rel = strings.Trim(rel, "/")
	if rel == "" {
		return bc
	}
	cur := ""
	for _, seg := range strings.Split(rel, "/") {
		if cur == "" {
			cur = seg
		} else {
			cur = cur + "/" + seg
		}
		bc = append(bc, Breadcrumb{Name: seg, Path: cur})
	}
	return bc
}

// parentRel 返回相对路径的上级目录（空表示根）
func parentRel(rel string) string {
	rel = strings.Trim(rel, "/")
	if rel == "" {
		return ""
	}
	if idx := strings.LastIndex(rel, "/"); idx >= 0 {
		return rel[:idx]
	}
	return ""
}

// 常见文本文件扩展名
var textExts = map[string]bool{
	".txt": true, ".md": true, ".log": true, ".go": true, ".c": true,
	".h": true, ".py": true, ".js": true, ".ts": true, ".json": true,
	".yaml": true, ".yml": true, ".xml": true, ".html": true, ".htm": true,
	".css": true, ".sh": true, ".conf": true, ".cfg": true, ".ini": true,
	".csv": true, ".toml": true, ".env": true, ".rs": true, ".java": true,
	".sql": true, ".properties": true,
}

// isTextFile 判断文件是否为可读文本（按扩展名 + 内容探测）
func isTextFile(path string) bool {
	if textExts[strings.ToLower(filepath.Ext(path))] {
		return true
	}
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()

	buf := make([]byte, 512)
	n, _ := f.Read(buf)
	for _, b := range buf[:n] {
		if b == 0 {
			return false
		}
		if b < 0x20 && b != '\n' && b != '\r' && b != '\t' && b != '\f' {
			return false
		}
	}
	return true
}
