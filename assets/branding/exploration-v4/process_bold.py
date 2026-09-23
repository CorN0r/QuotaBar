# -*- coding: utf-8 -*-
# v4「渐变进度环」加粗定稿（2026-09-22 晚）：出图原稿 source-raw-bold-1024.png
# （1024×1024，内容居中占 80%，无需裁剪）→ 清理：
#   ① alpha<64 噪点归零（约 1 万杂质像素）
#   ② alpha MinFilter(3) 腐蚀 1px —— 去除贴边品红色差描边（边缘 mask 检测确认）
#   ③ alpha GaussianBlur(0.6) 回软，重建抗锯齿坡道
# RGB 通道不动。→ ../quotabar-master.png
from PIL import Image, ImageFilter
import numpy as np

SRC = 'source-raw-bold-1024.png'
OUT = '../quotabar-master.png'

im = Image.open(SRC).convert('RGBA')
r, g, b = im.split()[0], im.split()[1], im.split()[2]
a = im.split()[3]
arr = np.array(im).astype(np.int32)
arr[:, :, 3] = np.where(arr[:, :, 3] < 64, 0, arr[:, :, 3])
im = Image.fromarray(arr.astype(np.uint8))
a = im.getchannel('A').filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(0.6))
im.putalpha(a)
im.save(OUT)

# 复检
m = np.array(im)
r2, g2, b2, a2 = m[:, :, 0].astype(int), m[:, :, 1].astype(int), m[:, :, 2].astype(int), m[:, :, 3].astype(int)
mag = (r2 > 200) & (g2 < 120) & (b2 > 150) & (a2 > 0)
vis = a2 >= 64
print('可见像素(alpha>=64):', int(vis.sum()))
print('残留品红(可见):', int((mag & vis).sum()))
print('alpha 1-63:', int(((a2 >= 1) & (a2 < 64)).sum()))
op = a2 >= 200
ys, xs = np.nonzero(op)
print('opaque bbox:', xs.min(), xs.max(), ys.min(), ys.max())
