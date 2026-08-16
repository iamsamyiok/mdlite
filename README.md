# MDLite

极简 Markdown 编辑器，纯 C + Win32 原生 API + GDI 直绘，单个 exe 仅 108 KB。

- 下载页：见仓库根 `index.html`（GitHub Pages 发布后即在线下载站）
- 源码：[`mdlite/`](./mdlite/)
- 构建：MinGW-w64（`x86_64-w64-mingw32-gcc`），进入 `mdlite/` 执行 `make`

## 特性速览

- 三态视图：编辑 / 分栏 / 预览（Ctrl+/ 切换）
- 内置 Markdown 渲染引擎（无浏览器内核）
- 内置极简 git 引擎：`.mdlite-git` 真仓库时间机，历史一键恢复
- AI 流式问答（`/` 命令）与 opencode Agent 调用（`//` 命令）
- 全局热键、自动保存、查找、大纲、HTML 导出
- Wine 兼容：Linux/Wine 下自动切换内置 CJK 字体，中文无乱码

## 目录

| 路径 | 内容 |
|------|------|
| `mdlite/` | C 源码与构建脚本 |
| `index.html` / `mdlite.b64.js` | 下载站页面（exe 以 base64 内嵌） |
| `edit.png` `split.png` `preview.png` | 下载站截图 |
