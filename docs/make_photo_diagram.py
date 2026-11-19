#!/usr/bin/env python3
"""Block diagram built from photos of the real bench (docs/photos/parts/*.jpg) -> docs/photo_block_diagram.png"""
import os
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
from PIL import Image

D = os.path.dirname(os.path.abspath(__file__)); P = os.path.join(D, "photos", "parts")
BG, CARD, TXT, SUB, BLUE = "#0f1720", "#1b2733", "#e6edf3", "#aab7c4", "#3498db"
fig, ax = plt.subplots(figsize=(20, 10.5), dpi=110); ax.set_xlim(0, 200); ax.set_ylim(0, 106); ax.axis("off")
fig.patch.set_facecolor(BG)
ax.text(100, 101, "LR2021 FLRC on Linux  -  the real hardware", ha="center", color="white", fontsize=24, weight="bold")
ax.text(100, 96, "Semtech LR2021 (Waveshare Core2021-HF) on Luckfox Pico boards, 2.44 GHz FLRC, 2600 kbit/s, FEC 3/4",
        ha="center", color="#9cc9ff", fontsize=13)

def group(x, y, w, h, title, col):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.4,rounding_size=1.5", fc=CARD, ec=col, lw=3))
    ax.text(x + w / 2, y + h - 3, title, ha="center", color=col, fontsize=15, weight="bold")

def photo(name, x, y, w, h, caption, sub=""):
    im = Image.open(os.path.join(P, name + ".jpg")); r = im.width / im.height
    if w / h > r: w2, h2 = h * r, h
    else: w2, h2 = w, w / r
    x0, y0 = x + (w - w2) / 2, y + (h - h2) / 2
    ax.imshow(im, extent=(x0, x0 + w2, y0, y0 + h2), zorder=3)
    ax.text(x + w / 2, y - 3, caption, ha="center", color=TXT, fontsize=12, weight="bold")
    if sub: ax.text(x + w / 2, y - 6.5, sub, ha="center", color=SUB, fontsize=10)
    return x + w / 2, y + h / 2

def arrow(p, q, label="", sub="", col="white", dashed=False, both=True, dy=2.5):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle="<|-|>" if both else "-|>", mutation_scale=26, color=col, lw=3,
                                 linestyle="--" if dashed else "-", zorder=4))
    mx, my = (p[0] + q[0]) / 2, (p[1] + q[1]) / 2
    if label: ax.text(mx, my + dy, label, ha="center", color=col, fontsize=13, weight="bold")
    if sub: ax.text(mx, my - dy - 2, sub, ha="center", color=SUB, fontsize=10)

# one horizontal chain, photos at the same height
Y, H = 44, 30
group(2, 22, 96, 64, "NODE A  -  camera / remote", "#f39c12")
group(102, 22, 96, 64, "NODE B  -  gateway", "#2ecc71")
photo("cam", 5, Y, 26, H, "SC3336 camera", "MIPI CSI")
photo("mini", 36, Y, 26, H, "Luckfox Pico Mini B", "RV1103 Linux")
photo("mini_mod", 67, Y, 28, H, "Core2021-HF", "Semtech LR2021")
photo("plus_mod", 105, Y, 28, H, "Core2021-HF", "Semtech LR2021")
photo("plus", 138, Y, 34, H, "Luckfox Pico Plus", "RV1103 Linux, RJ45")
arrow((31.5, Y + H / 2), (35.5, Y + H / 2), "CSI", both=False, dy=2)
arrow((62.5, Y + H / 2), (66.5, Y + H / 2), "SPI", dy=2)
arrow((133.5, Y + H / 2), (137.5, Y + H / 2), "SPI", dy=2)
arrow((95.5, Y + H / 2), (104.5, Y + H / 2), "", col=BLUE, dashed=True)
ax.text(100, 90.5, "FLRC 2.44 GHz radio link", ha="center", color=BLUE, fontsize=14, weight="bold")
ax.text(100, 87.6, "2600 kbit/s, CR 3/4, 0 dBm", ha="center", color=SUB, fontsize=10)
ax.text(50, 28, "vtx: camera RTSP stream -> H.265 -> radio frames     net: rf0 = 192.168.50.3, web server",
        ha="center", color=TXT, fontsize=11)
ax.text(150, 28, "vrx: radio -> video to PC     net: bridge eth0 <-> radio (MASTER)", ha="center", color=TXT, fontsize=11)
ax.add_patch(FancyBboxPatch((176, 50), 20, 18, boxstyle="round,pad=0.3,rounding_size=1", fc=CARD, ec=BLUE, lw=2.5, zorder=3))
ax.text(186, 61, "PC", ha="center", va="center", color=BLUE, fontsize=16, weight="bold", zorder=4)
ax.text(186, 55, "ffplay, browser\nping, dashboard", ha="center", va="center", color=TXT, fontsize=9, zorder=4)
arrow((172.5, Y + H / 2), (175.5, Y + H / 2), "", dy=2)
ax.text(100, 12, "Wiring (both nodes): CS 1C0_D, SCK 1C1_D, MOSI 1C2_D, MISO 1C3_D, BUSY 1C6_D, RESET 1C7_D, 3V3_OUT, GND",
        ha="center", color=SUB, fontsize=11)
ax.text(100, 7, "Photos of the actual bench. Antennas: 2.4 GHz whips on IPEX-to-SMA pigtails.", ha="center", color=SUB, fontsize=10)
fig.savefig(os.path.join(D, "photo_block_diagram.png"), facecolor=BG, bbox_inches="tight", pad_inches=0.2)
print("ok")
