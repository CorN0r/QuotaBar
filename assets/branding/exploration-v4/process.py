# -*- coding: utf-8 -*-
# 2026-09-22 v4「渐变进度环」母版加工：
# 出图原稿 source-raw-1254.png（1254×1254，AI 透明底）→ 清理 alpha<64 噪点
# → 按 alpha>=64 内容框裁剪 → 等比缩放至 776px（占画布 ~76%，与原稿占比一致）
# → 居中贴到 1024×1024 透明画布 → ../quotabar-master.png
from PIL import Image
import numpy as np

SRC = 'source-raw-1254.png'
OUT = '../quotabar-master.png'

im = Image.open(SRC).convert('RGBA')
arr = np.array(im).astype(np.int32)
a = arr[:, :, 3]
arr[:, :, 3] = np.where(a < 64, 0, a)

ys, xs = np.nonzero(arr[:, :, 3] >= 64)
x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
side = max(x1 - x0, y1 - y0)
cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
h = side // 2
crop = arr[max(0, cy - h):cy + h, max(0, cx - h):cx + h]
ch, cw = crop.shape[:2]
crop = crop[:min(ch, cw), :min(ch, cw)]

content = Image.fromarray(crop.astype(np.uint8)).resize((776, 776), Image.LANCZOS)
master = Image.new('RGBA', (1024, 1024), (0, 0, 0, 0))
master.paste(content, ((1024 - 776) // 2, (1024 - 776) // 2), content)
master.save(OUT)

# 品牌色采样：沿圆环中线按角度取点（右=蓝 / 左下=紫 / 左上=灰轨），绿点取绿色质心
import math
m = np.array(master)
C, R = 512.0, 328.0  # 环心 / 中线半径
def sample(deg):
    rad = math.radians(deg)
    x, y = int(C + R * math.cos(rad)), int(C - R * math.sin(rad))
    px = m[y, x]
    return f'#{px[0]:02X}{px[1]:02X}{px[2]:02X} a={px[3]}'
print('blue(0°右侧环):', sample(0))
print('violet(225°左下环):', sample(225))
print('track(135°左上灰轨):', sample(135))
g = m[:, :, 1]; r = m[:, :, 0]; b = m[:, :, 2]; al = m[:, :, 3]
gm = (g > 150) & (r < 150) & (al >= 200)
gys, gxs = np.nonzero(gm)
gx, gy = int(gxs.mean()), int(gys.mean())
gp = m[gy, gx]
print(f'green(指示点@{gx},{gy}): #{gp[0]:02X}{gp[1]:02X}{gp[2]:02X} a={gp[3]}')

# 复检：全图不应再有 alpha 1..63 的杂质
hist = np.histogram(m[:, :, 3], bins=[0, 1, 64, 256])[0]
print('alpha=0:', hist[0], ' alpha1-63(应为0):', hist[1], ' alpha>=64:', hist[2])
