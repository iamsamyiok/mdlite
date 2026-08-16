# MDLite

极简 Windows Markdown 编辑 / 查看器。单 exe、约 41 KB、零依赖、内存占用极低（空文档约 2-3 MB 工作集）。

## 构成

| 文件 | 说明 |
|------|------|
| `main.c` | 窗口、自绘界面、文件 IO、编辑控件 |
| `markdown.c/h` | Markdown 解析 + GDI 渲染（无浏览器内核） |
| `Makefile` | mingw-w64 交叉编译 |

## 功能

- 编辑：多行等宽编辑器，Tab 缩进（4 空格），Ctrl+A/C/V/X/Z
- 查看：内置渲染器，支持标题 H1-H6、粗体、斜体、行内代码、删除线、
  链接、图片占位、引用、有序/无序列表、代码块、分隔线、自动换行
- 文件：打开 / 保存 / 另存为 / 新建（UTF-8，兼容 BOM），支持拖拽
  .md 文件到窗口打开，支持命令行 `MDLite.exe note.md`
- 界面：浅色简洁风格，编辑/预览分段切换，状态栏显示行数、字符数、
  保存状态

## 快捷键

| 键 | 功能 |
|----|------|
| Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S | 新建 / 打开 / 保存 / 另存为 |
| Ctrl+/ | 切换 编辑 / 预览 |
| Tab | 插入 4 空格缩进 |
| Esc | 预览模式返回编辑 |

## 编译

```bash
# 依赖: mingw-w64 (gcc-mingw-w64-x86-64)
make            # 产出 MDLite.exe (64 位)
make test       # 解析与渲染冒烟测试 (需 wine)
```

仅在 Windows 10/11 64 位上测试通过；DPI 感知（Per-Monitor V2）。
