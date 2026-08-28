# MDLite

极简 Markdown 编辑器，纯 C + Win32 原生 API + GDI 直绘，单个 exe 仅 132 KB。

- 下载页：见仓库根 `index.html`（GitHub Pages 发布后即在线下载站）
- 源码：[`mdlite/`](./mdlite/)
- 构建：MinGW-w64（`x86_64-w64-mingw32-gcc`），进入 `mdlite/` 执行 `make`
- 测试：`make test`（wine 跑解析/渲染/像素断言）

## 特性速览

- 三态视图：编辑 / 分栏 / 预览（Ctrl+/ 切换）
- 内置 Markdown 渲染引擎（无浏览器内核），支持 GFM 表格与任务列表
- 内置极简 git 引擎：exe 旁 `.mdlite\history\` 统一存储真仓库时间机，历史一键恢复、超限物理截断、旧布局自动迁移
- AI 流式问答（`/` 命令）与 opencode Agent 调用（`//` 命令）
- 外部修改检测：文件被其他程序改动后自动重载或状态栏提示
- 全局热键、自动保存、查找、大纲、HTML 导出、打印
- Wine 兼容：Linux/Wine 下自动切换内置 CJK 字体，中文无乱码

## 目录

| 路径 | 内容 |
|------|------|
| `mdlite/` | C 源码与构建脚本 |
| `index.html` / `mdlite.b64.js` | 下载站页面（exe 以 base64 内嵌） |
| `edit.png` `split.png` `preview.png` | 下载站截图 |
| `.github/workflows/ci.yml` | CI：MinGW 交叉编译 + wine 跑测试 |
