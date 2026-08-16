#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate the MDLite app icon: rounded light square + bold blue M."""
from PIL import Image, ImageDraw, ImageFont

BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

def make(sz):
    img = Image.new("RGBA", (sz, sz), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = max(4, sz * 60 // 256)
    d.rounded_rectangle([0, 0, sz - 1, sz - 1], radius=r,
                        fill=(245, 245, 247, 255),
                        outline=(210, 210, 215, 255),
                        width=max(1, sz // 64))
    f = ImageFont.truetype(BOLD, int(sz * 0.66))
    bbox = d.textbbox((0, 0), "M", font=f)
    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]
    d.text(((sz - w) / 2 - bbox[0], (sz - h) / 2 - bbox[1]), "M",
           font=f, fill=(0, 122, 255, 255))
    return img

make(256).save("/workspace/mdlite/icon.ico",
               sizes=[(16, 16), (32, 32), (48, 48), (256, 256)])
print("icon.ico saved")
