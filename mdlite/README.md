# MDLite

极简 Windows Markdown 编辑 / 查看器。单 exe、约 146 KB、零依赖、内存占用极低（空文档约 2-3 MB 工作集）。

## 构成

| 文件 | 说明 |
|------|------|
| `main.c` | 窗口、自绘界面、文件 IO、编辑控件、设置、历史面板 |
| `markdown.c/h` | Markdown 解析 + GDI 渲染（含 GFM 表格、任务列表） |
| `gitlite.c` | 极简 git 引擎（快照、压缩存储、物理截断、旧布局迁移） |
| `zliblite.c/h` | 零依赖 zlib（deflate/inflate）实现 |
| `ai.c` | AI 流式问答 + opencode Agent 后台任务 |
| `export.c` | HTML / 纯文本导出与打印 |
| `mdlite.h` | 跨模块共享声明 |
| `test.c` / `testrender.c` | 解析与渲染测试（wine 运行） |
| `Makefile` | mingw-w64 交叉编译 |

## 构建与测试

```bash
make          # 产出 MDLite.exe（零警告）
make test     # wine 跑 test.exe + testrender.exe
```

## 功能

- 编辑：多行等宽编辑器，Tab 缩进（4 空格），Ctrl+A/C/V/X/Z
- 查看：内置渲染器，支持标题 H1-H6、粗体、斜体、行内代码、删除线、
  链接、图片渲染（PNG/JPG/BMP/GIF，WIC 解码）、脚注、引用、有序/无序列表、任务列表、GFM 表格（列对齐）、
  代码块、分隔线、自动换行
- 编辑增强：Ctrl+D 复制当前行、Ctrl+L 选整行、Alt+Shift+↑↓ 交换行、
  Tab/Shift+Tab 整体缩进/反缩进选区、回车自动续列表行
- 查找替换：Ctrl+H 弹窗，Enter 逐次替换、Ctrl+Enter 全部替换
- 撤销重做：RichEdit50W 原生多步（msftedit.dll 加载失败自动回退 EDIT）
- 文件：打开 / 保存 / 另存为 / 新建（UTF-8，兼容 BOM），支持拖拽
  .md 文件到窗口打开，外部修改自动重载（干净时）或状态栏提示
- 历史：exe 旁 `.mdlite\history\` 统一存储，真 git 仓库、zlib 压缩、
  超限物理截断、钉住保护、旧 `.mdlite-git` 自动迁移
- AI：行首 `/` 流式问答（多轮上下文），`//` 调用 opencode Agent
- 界面：浅色简洁风格，编辑/预览分段切换，状态栏行:列、字数、阅读时长

## 快捷键

| 键 | 功能 |
|----|------|
| Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S | 新建 / 打开 / 保存 / 另存为 |
| Ctrl+/ | 切换 编辑 / 分栏 / 预览 |
| Ctrl+F / Ctrl+H / Ctrl+P | 查找 / 查找替换 / 文档大纲 |
| Ctrl+D / Ctrl+L | 复制当前行 / 选整行 |
| Alt+Shift+↑ / ↓ | 交换当前行与上下行 |
| Ctrl+Z / Ctrl+Y | 撤销 / 重做 |
| Ctrl+J | 选区 AI 指令 |
| Tab | 插入 4 空格缩进 |
| Esc | 预览模式返回编辑 / 中断 AI |
