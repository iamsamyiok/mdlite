#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate clean marketing screenshots for MDLite (no wine artifacts).
Replicates the exact UI spec: colors, sizes, radii, heading ladder."""
from PIL import Image, ImageDraw, ImageFont

W, H = 904, 613
HEADER_H, STATUS_H = 48, 26

# Apple-style palette
BG_HEADER = (245, 245, 247)
BORDER   = (210, 210, 215)
LABEL    = (29, 29, 31)
SEC      = (110, 110, 115)
SEC2     = (134, 134, 139)
ACCENT   = (0, 122, 255)
SEG_BG   = (232, 232, 237)
CODE_BG  = (245, 245, 247)
CODE_INL = (240, 240, 244)
QUOTE_BAR= (201, 201, 206)
RULE     = (232, 232, 237)
WHITE    = (255, 255, 255)

CJK = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
CJK_B = "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"
MONO = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
MONO_B = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
ICON = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

def cjk(sz, bold=False):
    return ImageFont.truetype(CJK_B if bold else CJK, sz)

def mono(sz, bold=False):
    return ImageFont.truetype(MONO_B if bold else MONO, sz)

def icon_f(sz):
    return ImageFont.truetype(ICON, sz)

def tw(f, s):
    return f.getlength(s)

def draw_mixed(d, x, y, s, f_ascii, f_cjk, fill):
    """draw mixed ascii/cjk text, returns end x"""
    cx = x
    buf = ""
    cur_cjk = None
    def flush():
        nonlocal cx, buf
        if not buf: return
        f = f_cjk if cur_cjk else f_ascii
        d.text((cx, y), buf, font=f, fill=fill)
        cx += f.getlength(buf)
        buf = ""
    for ch in s:
        is_cjk = ord(ch) > 0x2000 or (0x1100 <= ord(ch) <= 0x11FF)
        if cur_cjk is None: cur_cjk = is_cjk
        if is_cjk != cur_cjk:
            flush(); cur_cjk = is_cjk
        buf += ch
    flush()
    return cx

def draw_header(d, view):
    """view: 0=edit 1=split 2=preview"""
    d.rectangle([0, 0, W, HEADER_H], fill=BG_HEADER)
    d.line([(0, HEADER_H - 1), (W, HEADER_H - 1)], fill=BORDER, width=1)
    # pill buttons (icons drawn with DejaVuSans: ▶■☰⚙ all have real glyphs there)
    fi = icon_f(13)
    for i, (icon, label) in enumerate(
            [(u"\u25B6", u"打开"), (u"\u25A0", u"保存"), (u"\u2630", u"历史"),
             (u"\u2699", u"配置")]):
        x0 = 14 + i * 72
        d.rounded_rectangle([x0, 10, x0 + 64, 38], radius=14,
                            fill=WHITE, outline=BORDER, width=1)
        f = cjk(13)
        iw = fi.getlength(icon)
        lw = f.getlength(" " + label)
        tx = x0 + (64 - iw - lw) / 2
        d.text((tx, 12), icon, font=fi, fill=LABEL)
        d.text((tx + iw, 12), " " + label, font=f, fill=LABEL)
    # centered file name
    f = cjk(13, bold=True)
    name = "sample.md"
    d.text(((W - tw(f, name)) / 2, 12), name, font=f, fill=LABEL)
    # three-segment switch
    sw = 180
    sx = W - 14 - sw
    d.rounded_rectangle([sx, 10, sx + sw, 38], radius=14, fill=SEG_BG)
    third = sw / 3
    act_x = sx + third * view + 1
    d.rounded_rectangle([act_x, 12, act_x + third - 3, 36], radius=11,
                        fill=WHITE, outline=BORDER, width=1)
    f = cjk(13)
    for i, label in enumerate([u"编辑", u"分栏", u"预览"]):
        cx = sx + third * i + (third - tw(f, label)) / 2
        d.text((cx, 12), label, font=f,
               fill=ACCENT if i == view else SEC)

def draw_status(d, nlines, nchars, dirty):
    top = H - STATUS_H
    d.rectangle([0, top, W, H], fill=BG_HEADER)
    d.line([(0, top), (W, top)], fill=BORDER, width=1)
    f = cjk(12)
    left = u"行 %d   字符 %d" % (nlines, nchars)
    d.text((14, top + 6), left, font=f, fill=SEC2)
    right = (u"未保存" if dirty else u"已保存") + "   UTF-8"
    d.text((W - 14 - tw(f, right), top + 6), right, font=f,
           fill=ACCENT if dirty else SEC2)

def make_edit():
    img = Image.new("RGB", (W, H), WHITE)
    d = ImageDraw.Draw(img)
    # source lines shown in the editor
    src = [
        (u"# MDLite 用户手册", "h"),
        (u"", ""),
        (u"这是一段**加粗正文**，还有 `行内代码` 与", ""),
        (u"[链接](https://example.com)。", ""),
        (u"", ""),
        (u"## 快速上手", "h"),
        (u"", ""),
        (u"双击 exe 即可运行，把 .md 文件拖进窗口就能打开。", ""),
        (u"", ""),
        (u"## 功能清单", "h"),
        (u"", ""),
        (u"- 三态视图：编辑 / 分栏 / 预览（Ctrl+/）", ""),
        (u"- Ctrl+F 查找 · Ctrl+滚轮缩放", ""),
        (u"- 自动历史存档，一键恢复任意版本", ""),
        (u"- 可配置全局热键 · 可配置自动保存", ""),
        (u"", ""),
        (u"> 设计目标：单文件 74KB，内存 3MB。", ""),
        (u"", ""),
        (u"```c", ""),
        (u"int main(void) {", ""),
        (u"    printf(\"hello md\");", ""),
        (u"    return 0;", ""),
        (u"}", ""),
        (u"```", ""),
    ]
    y = HEADER_H + 10
    x0 = 16
    for line, kind in src:
        if line:
            if kind == "h":
                draw_mixed(d, x0, y, line, mono(13, True), cjk(13, True), LABEL)
            else:
                draw_mixed(d, x0, y, line, mono(13), cjk(13), LABEL)
        y += 20
    draw_header(d, view=0)
    draw_status(d, 23, 486, False)
    return img

def code_block(d, x, y, w, lines):
    lh = 19
    pad = 10
    h = pad * 2 + lh * len(lines)
    d.rounded_rectangle([x, y, x + w, y + h], radius=14, fill=CODE_BG)
    cy = y + pad
    for ln in lines:
        draw_mixed(d, x + 14, cy, ln, mono(13), cjk(13), LABEL)
        cy += lh
    return y + h

def inline_code(d, x, y, s, f_a, f_c):
    w = f_a.getlength(s) if s.isascii() else f_c.getlength(s)
    d.rounded_rectangle([x, y, x + w + 8, y + 20], radius=6, fill=CODE_INL)
    draw_mixed(d, x + 4, y + 1, s, f_a, f_c, LABEL)
    return x + w + 8

def make_preview():
    img = Image.new("RGB", (W, H), WHITE)
    d = ImageDraw.Draw(img)
    M = 24
    cw = W - M * 2
    y = HEADER_H  # document y=0 starts at client top
    f_body = cjk(14); f_bold = cjk(14, True)
    lh_body = 21

    def heading(text, px, above, below, rule=False):
        nonlocal y
        y += above
        f = cjk(px, True)
        d.text((M, y), text, font=f, fill=LABEL)
        y += px + 2
        if rule:
            d.line([(M, y + below - 3), (M + cw, y + below - 3)], fill=RULE, width=1)
        y += below

    # H1
    heading(u"MDLite 用户手册", 35, 22, 12, rule=True)
    # paragraph with inline styles
    y += 2
    x = M
    x = draw_mixed(d, x, y, u"这是一段", f_body, f_body, LABEL)
    x = draw_mixed(d, x, y, u"加粗正文", mono(14), cjk(14, True), LABEL)
    x = draw_mixed(d, x, y, u"，还有 ", f_body, f_body, LABEL)
    x = inline_code(d, x, y + 1, u"行内代码", mono(13), cjk(13))
    x = draw_mixed(d, x, y, u" 与 ", f_body, f_body, LABEL)
    d.text((x, y), u"链接", font=f_body, fill=ACCENT)
    uw = tw(f_body, u"链接")
    d.line([(x, y + 17), (x + uw, y + 17)], fill=ACCENT, width=1)
    y += lh_body + 8
    # H3..H6 ladder demo
    heading(u"标题层级 H3", 21, 10, 6)
    heading(u"标题层级 H4", 18, 8, 5)
    heading(u"标题层级 H5", 16, 7, 4)
    f6 = cjk(14, True)
    y += 6
    d.text((M, y), u"标题层级 H6", font=f6, fill=SEC)
    y += 14 + 8
    # H2
    heading(u"功能清单", 27, 16, 8, rule=True)
    # list
    for item in [u"三态视图：编辑 / 分栏 / 预览（Ctrl+/）",
                 u"Ctrl+F 查找 · Ctrl+滚轮缩放",
                 u"自动历史存档，一键恢复任意版本"]:
        d.text((M + 2, y - 1), u"\u2022", font=f_bold, fill=LABEL)
        draw_mixed(d, M + 20, y, item, f_body, f_body, LABEL)
        y += lh_body + 2
    y += 8
    # quote
    d.rounded_rectangle([M + 1, y - 2, M + 4, y + 19], radius=2, fill=QUOTE_BAR)
    draw_mixed(d, M + 18, y, u"设计目标：单文件 74KB，内存 3MB。", f_body, f_body, SEC)
    y += lh_body + 12
    # code block
    y = code_block(d, M, y, cw, [u"int main(void) {",
                                 u'    printf("hello md");',
                                 u"    return 0;",
                                 u"}"])
    draw_header(d, view=2)
    draw_status(d, 23, 486, False)
    return img

def make_split():
    """split view: rendered preview on the left, source on the right"""
    img = Image.new("RGB", (W, H), WHITE)
    d = ImageDraw.Draw(img)
    mid = W // 2
    # left pane reuses the preview layout, narrower
    M = 18
    cw = mid - M * 2
    y = HEADER_H
    f_body = cjk(13); f_bold = cjk(13, True)
    lh = 19

    def heading(text, px, above, below, rule=False):
        nonlocal y
        y += above
        f = cjk(px, True)
        d.text((M, y), text, font=f, fill=LABEL)
        y += px + 2
        if rule:
            d.line([(M, y + below - 3), (M + cw, y + below - 3)], fill=RULE, width=1)
        y += below

    heading(u"MDLite 用户手册", 30, 18, 10, rule=True)
    x = M
    x = draw_mixed(d, x, y, u"这是一段", f_body, f_body, LABEL)
    x = draw_mixed(d, x, y, u"加粗正文", mono(13), cjk(13, True), LABEL)
    x = draw_mixed(d, x, y, u"，还有 ", f_body, f_body, LABEL)
    x = inline_code(d, x, y + 1, u"行内代码", mono(12), cjk(12))
    y += lh + 8
    heading(u"功能清单", 23, 12, 7, rule=True)
    for item in [u"三态视图（Ctrl+/）", u"全局热键 · 自动保存", u"自动历史存档"]:
        d.text((M + 2, y - 1), u"\u2022", font=f_bold, fill=LABEL)
        draw_mixed(d, M + 18, y, item, f_body, f_body, LABEL)
        y += lh + 2
    y += 6
    code_block(d, M, y, cw, [u"int main(void) {",
                             u'    printf("hello md");',
                             u"    return 0;",
                             u"}"])

    # center divider
    d.line([(mid, HEADER_H), (mid, H - STATUS_H)], fill=BORDER, width=1)
    # right pane: source
    rx = mid + 14
    src = [
        (u"# MDLite 用户手册", "h"),
        (u"", ""),
        (u"这是一段**加粗正文**，还有", ""),
        (u"`行内代码` 与 [链接](…)", ""),
        (u"", ""),
        (u"## 功能清单", "h"),
        (u"", ""),
        (u"- 三态视图（Ctrl+/）", ""),
        (u"- 全局热键 · 自动保存", ""),
        (u"- 自动历史存档", ""),
        (u"", ""),
        (u"```c", ""),
        (u"int main(void) {", ""),
        (u'    printf("hello md");', ""),
        (u"}", ""),
        (u"```", ""),
    ]
    y = HEADER_H + 12
    for line, kind in src:
        if line:
            if kind == "h":
                draw_mixed(d, rx, y, line, mono(13, True), cjk(13, True), LABEL)
            else:
                draw_mixed(d, rx, y, line, mono(13), cjk(13), LABEL)
        y += 20
    draw_header(d, view=1)
    draw_status(d, 23, 486, False)
    return img

make_edit().save("/workspace/show-page/edit.png")
make_preview().save("/workspace/show-page/preview.png")
make_split().save("/workspace/show-page/split.png")
import shutil
for n in ("edit.png", "preview.png", "split.png"):
    shutil.copy("/workspace/show-page/" + n, "/workspace/show/" + n)
print("saved")
