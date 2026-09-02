## Goal
- 按 OpenKnowledge 对比结论逐步为 MDLite（纯 C + Win32 + GDI 的极简 Markdown 编辑器）实施功能升级：第一批（置顶按钮、@ 插入菜单、自包含 HTML 分享导出）、第二批（文件树、wiki-link/backlink 孤儿检测）、第三批（多标签、graph 可视化）

## Constraints & Preferences
- 触发指令改为 `@`（`/` 与 `//` 已被 AI 问答/Agent 占用）
- 保持"单 exe 极小、零依赖、零警告编译"定位；wine 下中文无乱码
- 不做在线 share-link（需服务器、违背本地隐私定位），以自包含 HTML 导出替代
- 每步编译验证：`-Wall -Wextra` 零警告

## Progress
### Done
- 前期研究：open-knowledge 与 MDLite 全面对比；7 项功能难度/体积评估与实施路线
- 环境搭建：`gcc-mingw-w64-x86-64` 已装好；wine 可用
- #4 置顶按钮上主栏：BtnRect 特判 id==5；HitTestHeader 循环扩容；DrawHeader 画"置顶"；点击 SetTopmost
- #7 @ 插入菜单：main.c 新增 InsItem + INS_ITEMS 17 项；InsFill/InsApply/Filter/Proc 完整；mdlite.h 声明；ai.c EditProc WM_CHAR 拦截 @
- #5 自包含 HTML 导出：markdown.c 加 h_img_data_uri/h_app_b64/md_to_html_standalone；export.c ExportShareHtml；test.c 内嵌图片测试
- #1 文件树：tree.c/tree.h 完整（TreeNode 链表树、惰性 EnumChildren、Flatten、自绘、宽度/显隐持久化）；main.c 集成全部到位
- #3 wiki-link + 反向链接/孤儿：markdown.h RF_WIKILINK=256 + LinkSink 签名；markdown.c parse_inline/h_inline wiki 分支；main.c LinkSink/OpenLink/OpenWikiLink + 链接面板；tree.c TreeScanLinks/TreeBacklinks/TreeOrphans/TreeJumpResolve；test.c 6 项新测试
- 帮助文本 + README 已更新
- 全量测试通过：test.exe + testrender.exe ALL PASS，零警告

### In Progress
- 无

### Blocked
- 无（wine test 已可跑）

## Key Decisions
- @ 而非 / 触发插入菜单
- 树用自绘而非 comctl32 TreeView
- 工作区 = 当前文档所在目录；侧栏默认收起
- wiki-link url 前缀 hack：parse_inline 直接存 target，md_paint sink 传 wiki 标志给 OpenLink
- wiki 跳转找到不存在时提供"创建笔记"选项
- 链接面板无 filter（v1 简单为主），仅显示反链+孤儿
- wiki run 被排版拆成多 run（空格分段）正常，测试按 url 统计而非 run 计数

## Next Steps
- 第三批按需启动：#2 多标签 / #6 graph 可视化
- 如需继续，按上次制定的路线进行

## Relevant Files
- /workspace/mdlite/main.c：主窗口、链接面板(LinksFill/ShowLinks/HoldLinks)、wiki 跳转
- /workspace/mdlite/tree.c + tree.h：文件树 + wiki 链接索引
- /workspace/mdlite/markdown.c + markdown.h：渲染引擎 wiki 解析
- /workspace/mdlite/test.c + testrender.c：单元测试
- /workspace/mdlite/Makefile：build targets
- /workspace/mdlite/README.md：文档更新
- /workspace/mdlite/ai.c：@ 拦截入口
