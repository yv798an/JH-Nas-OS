package main

import (
	"flag"
	"fmt"
	"log"
	"net/http"
)

func main() {
	root := flag.String("root", "", "NAS 存储根目录，多个用逗号分隔（如 -root /media/sda1,/media/sdb1；留空自动探测）")
	addr := flag.String("addr", ":8080", "监听地址")
	flag.Parse()

	nasRoots = parseRoots(*root)
	if len(nasRoots) == 0 {
		log.Fatal("未找到任何有效的 NAS 存储目录，请用 -root 指定，例如 -root /media/sda1,/media/sdb1")
	}

	if err := loadTemplates(); err != nil {
		log.Fatalf("加载模板失败: %v", err)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", handleIndex)
	mux.HandleFunc("/view", handleView)
	mux.HandleFunc("/raw", handleRaw)
	mux.HandleFunc("/download", handleDownload)
	mux.Handle("/static/", http.StripPrefix("/static/", http.FileServer(http.FS(staticSubFS))))

	fmt.Printf("✅ NAS 文件浏览器已启动\n")
	fmt.Printf("   地址:   http://localhost%s\n", *addr)
	for _, r := range nasRoots {
		fmt.Printf("   存储:   %s  →  %s\n", r.Name, r.Path)
	}
	log.Fatal(http.ListenAndServe(*addr, mux))
}
