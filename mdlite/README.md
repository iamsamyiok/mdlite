# MDLite

极简 Windows Markdown 编辑 / 查看器。单 exe、约 170 KB、零依赖、内存占用极低（空文档约 2-3 MB 工作集）。

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
- 文件树（VSCode 式）：Ctrl+B 切换左侧工作区文件列表；标题栏右侧
  工具按钮支持新建文件、新建文件夹、折叠全部；双击/Enter 打开文件，
  双击目录展开折叠；右键菜单支持打开、目录内新建文件/文件夹、重命名、
  复制文件地址/文件名、删除（进回收站）；命名框 Enter 确认、Esc 取消；
  侧栏宽度/显隐自动持久化
- 反向链接与孤儿笔记：Ctrl+Shift+L 弹出面板，列出引用当前文档的其他
  .md 文件（反向链接）和不被任何文件引用的 .md（孤儿），双击跳转；
  依赖 wiki-link 扫描，需先保存至少一个 .md 文件才有效
- Wiki-link：Markdown 中使用 `[[目标文件名或路径]]` 或
  `[[目标|显示名]]` 在预览中渲染为可点链接；点击自动在工作区解析
  并跳转，找不到时提供创建笔记的选项
- 双链自动补全：输入 `[[` 自动弹出工作区笔记列表（焦点留在编辑器），
  继续输入即过滤，↑↓ 选择、Enter/Tab 补全、Esc 关闭
- 斜杠命令（Notion 风格）：输入 `/` 弹出块命令菜单——任务/无序/有序
  列表、代码块、表格、引用块、提示框（Callout）、突出显示、帽头
  （frontmatter）、分隔线、今日日期（11 项）；输入即过滤，↑↓ 选择、
  Enter/Tab 应用；过滤无匹配自动收起，弹窗高度随条目数自适应
- 突出显示（Notion highlight）：整行用 `==` 包裹（`==重点内容==`）
  在预览与导出 HTML 中渲染为黄色高亮条，圆角色块横贯整行
- 插入菜单：编辑器按 `@` 弹出 Markdown 片段菜单（标题/表格/代码块/任务列表等），
  输入过滤、Enter 插入、Esc 关闭
- 括号配对：输入 `[` `(` `{` 自动补全配对并包裹选区；输入 `]` `)` `}`
  跳出配对；退格删除整对
- 提示框 Callout：`> [!NOTE]`、`[!TIP]`、`[!IMPORTANT]`、`[!WARNING]`、
  `[!CAUTION]`（GitHub alerts 语法）在预览与导出 HTML 中渲染为彩色卡片
- 知识图谱：`Ctrl+G` 进入/退出全库链接关系图（支持 `[[wiki]]` 和 `[text](target)` 两种语法，
  全盘扫描不依赖文件树展开状态）；力导向布局、滚轮缩放、拖拽节点、双击跳转；
  节点右上角显示连接数 badge；保存文档后自动重建
- 查找替换：Ctrl+H 弹窗，Enter 逐次替换、Ctrl+Enter 全部替换
- 撤销重做：RichEdit50W 原生多步（msftedit.dll 加载失败自动回退 EDIT）
- 文件：打开 / 保存 / 另存为 / 新建（UTF-8，兼容 BOM），支持拖拽
  .md 文件到窗口打开，外部修改自动重载（干净时）或状态栏提示
- 分享导出：更多菜单「导出分享 HTML」生成自包含单文件 HTML，
  本地图片（PNG/JPG/GIF/BMP/WEBP）base64 内嵌，任意浏览器直接查看，
  导出后自动复制路径并在资源管理器中定位
- 历史：exe 旁 `.mdlite\history\` 统一存储，真 git 仓库、zlib 压缩、
  超限物理截断、钉住保护、旧 `.mdlite-git` 自动迁移
- AI：行首 `//` 流式问答（多轮上下文），`///` 调用 opencode Agent
- 界面：浅色简洁风格，编辑/预览分段切换，标题栏「置顶」按钮一键窗口总在最前，
  状态栏行:列、字数、阅读时长

## 快捷键

| 键 | 功能 |
|----|------|
| Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S | 新建 / 打开 / 保存 / 另存为 |
| Ctrl+/ | 切换 编辑 / 分栏 / 预览 |
| Ctrl+F / Ctrl+H / Ctrl+P | 查找 / 查找替换 / 文档大纲 |
| Ctrl+B | 切换左侧文件树 |
| `@` | 插入 Markdown 片段菜单 |
| `/` | 斜杠命令菜单（块命令） |
| `//问题` + Enter | 问 AI（OpenAI 兼容接口） |
| `///任务` + Enter | Agent（opencode 后台任务） |
| `[[` | 双链笔记名自动补全 |
| F1 或 `?` 按钮 | 打开使用帮助 |
| Ctrl+Shift+L | 反向链接与孤儿笔记面板 |
| Ctrl+G | 进入 / 退出知识图谱 |
| Ctrl+D / Ctrl+L | 复制当前行 / 选整行 |
| Alt+Shift+↑ / ↓ | 交换当前行与上下行 |
| @ | 弹出 Markdown 片段插入菜单 |
| Ctrl+Z / Ctrl+Y | 撤销 / 重做 |
| Ctrl+J | 选区 AI 指令 |
| Tab | 插入 4 空格缩进 |
| Esc | 预览模式返回编辑 / 中断 AI |
