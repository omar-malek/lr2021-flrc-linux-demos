#!/usr/bin/env python3
"""Draws the diagrams used in the READMEs (matplotlib only, no external images).
   python3 docs/make_diagrams.py   -> docs/*.png"""
import os
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

OUT = os.path.dirname(os.path.abspath(__file__))
BG, CARD, TXT, SUB = "#0f1720", "#1b2733", "#e6edf3", "#aab7c4"
ORANGE, GREEN, BLUE, PURPLE, RED = "#f39c12", "#2ecc71", "#3498db", "#9b59b6", "#e74c3c"


def canvas(w, h):
    fig, ax = plt.subplots(figsize=(w / 10, h / 10), dpi=150)
    ax.set_xlim(0, w); ax.set_ylim(0, h); ax.axis("off"); fig.patch.set_facecolor(BG)
    return fig, ax


def box(ax, x, y, w, h, title, lines, col, fs=10):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.3,rounding_size=1.2", fc=CARD, ec=col, lw=2.2))
    ax.text(x + w / 2, y + h - 3.2, title, ha="center", va="center", color=col, fontsize=fs + 2, weight="bold")
    for i, l in enumerate(lines):
        ax.text(x + w / 2, y + h - 7.5 - i * 3.6, l, ha="center", va="center", color=TXT, fontsize=fs)


def arrow(ax, x1, y1, x2, y2, label="", sub="", col="white", both=False, dashed=False, dy=2.2):
    ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle="<|-|>" if both else "-|>", mutation_scale=18,
                                 color=col, lw=2.2, linestyle="--" if dashed else "-"))
    if label: ax.text((x1 + x2) / 2, (y1 + y2) / 2 + dy, label, ha="center", color=col, fontsize=10, weight="bold")
    if sub: ax.text((x1 + x2) / 2, (y1 + y2) / 2 - dy - 1.6, sub, ha="center", color=SUB, fontsize=8.5)


def save(fig, name):
    fig.savefig(os.path.join(OUT, name), facecolor=BG, bbox_inches="tight", pad_inches=0.3); plt.close(fig)


# 1. hardware block diagram (both boards)
fig, ax = canvas(160, 82)
ax.text(80, 78, "Hardware: two Linux boards, each with an LR2021 radio on SPI", ha="center", color="white", fontsize=15, weight="bold")
for x0, name, extra, col in ((4, "Board A  -  Luckfox Pico Mini B", ["SC3336 camera (MIPI CSI)", "H.265 encoder (rkipc / RTSP)"], ORANGE),
                             (84, "Board B  -  Luckfox Pico Plus", ["100 Mbit/s Ethernet (RJ45)", ""], GREEN)):
    box(ax, x0, 30, 72, 42, name, [], col, fs=10)
    box(ax, x0 + 3, 46, 30, 18, "RV1103 SoC", ["ARM Cortex-A7, Linux", "spidev + sysfs GPIO", "lr2021_flrc (this repo)"], SUB, fs=8)
    box(ax, x0 + 39, 46, 30, 18, "Waveshare Core2021-HF", ["Semtech LR2021", "32 MHz crystal", "2.4 GHz antenna (IPEX)"], PURPLE, fs=8)
    arrow(ax, x0 + 33.5, 55, x0 + 38.5, 55, both=True)
    ax.text(x0 + 36, 59.5, "SPI", ha="center", color="white", fontsize=8, weight="bold")
    ax.text(x0 + 36, 48.5, "BUSY\nRESET", ha="center", color=SUB, fontsize=7)
    for i, l in enumerate(extra):
        ax.text(x0 + 36, 40 - i * 4, l, ha="center", color=TXT, fontsize=9)
arrow(ax, 77, 52, 83, 52, col=BLUE, both=True, dashed=True)
ax.text(80, 46, "FLRC\n2.44 GHz", ha="center", color=BLUE, fontsize=9, weight="bold")
box(ax, 100, 2, 40, 20, "PC (Linux)", ["ffplay / browser / ping", "dashboards"], BLUE, fs=9)
arrow(ax, 120, 29.5, 120, 22.5, col="white", both=True)
ax.text(123, 26, "USB (adb) or RJ45", color="white", fontsize=9, weight="bold", va="center")
box(ax, 10, 2, 60, 20, "Power", ["Both boards powered from USB-C", "Radio module: 3V3_OUT of the board"], SUB, fs=9)
save(fig, "hardware_block_diagram.png")

# 2. wiring diagram
fig, ax = canvas(130, 92)
ax.text(65, 88, "Wiring  -  identical on both boards", ha="center", color="white", fontsize=15, weight="bold")
box(ax, 4, 6, 40, 76, "Luckfox Pico", ["(Mini B or Plus)"], GREEN, fs=10)
box(ax, 86, 6, 40, 76, "Core2021-HF", ["(LR2021 module)"], PURPLE, fs=10)
rows = [("1C0_D", "GPIO 48", "CS", ORANGE), ("1C1_D", "GPIO 49", "SCK / CLK", "#d4ac0d"), ("1C2_D", "GPIO 50", "MOSI", BLUE),
        ("1C3_D", "GPIO 51", "MISO", GREEN), ("1C6_D", "GPIO 54", "BUSY", "#17a589"), ("1C7_D", "GPIO 55", "RESET", PURPLE),
        ("3V3_OUT", "", "3.3V", RED), ("GND", "", "GND", "#95a5a6")]
for i, (pin, gp, sig, col) in enumerate(rows):
    y = 68 - i * 7.6
    ax.text(8, y, pin, color=TXT, fontsize=10, weight="bold", va="center")
    ax.text(26, y, gp, color=SUB, fontsize=9, va="center")
    ax.plot([42, 88], [y, y], color=col, lw=3); ax.plot([42, 88], [y, y], "o", color=col, ms=6)
    ax.text(92, y, sig, color=TXT, fontsize=10, weight="bold", va="center")
ax.text(65, 2, "SPI: /dev/spidev0.0, mode 0, 8 MHz (12 MHz also works). BUSY/RESET are on 1C6_D/1C7_D, not 1D0/1D1 (UART3).",
        ha="center", color=SUB, fontsize=8.5)
save(fig, "wiring.png")

# 3. video demo
fig, ax = canvas(170, 60)
ax.text(85, 56, "Demo 1  -  live H.265 video over FLRC", ha="center", color="white", fontsize=15, weight="bold")
box(ax, 2, 18, 30, 28, "Camera", ["SC3336", "rkipc H.265", "704x576, 30 fps", "RTSP 127.0.0.1"], ORANGE, fs=9)
box(ax, 44, 18, 34, 28, "lr2021_flrc vtx", ["RTSP -> RTP (RFC 7798)", "-> Annex-B stream", "-> 255 B air frames", "[A5 seq len] + 251 B"], ORANGE, fs=9)
box(ax, 92, 18, 34, 28, "lr2021_flrc vrx", ["read FIFO at once", "seq check + resync", "loss / CRC / RSSI", "stdout or UDP"], GREEN, fs=9)
box(ax, 138, 18, 30, 28, "PC", ["ffplay", "low-delay decode", "live_demo.py", "stats window"], BLUE, fs=9)
arrow(ax, 32.5, 32, 43.5, 32, "RTSP")
arrow(ax, 78.5, 32, 91.5, 32, "FLRC", "2600 kbit/s, CR 3/4", col=BLUE, dashed=True)
arrow(ax, 126.5, 32, 137.5, 32, "USB / UDP")
ax.text(61, 12, "Pico Mini B", ha="center", color=ORANGE, fontsize=10); ax.text(109, 12, "Pico Plus", ha="center", color=GREEN, fontsize=10)
save(fig, "video_block_diagram.png")

# 4. TCP/IP demo
fig, ax = canvas(170, 64)
ax.text(85, 60, "Demo 2  -  TCP/IP network over FLRC (half duplex, token passing)", ha="center", color="white", fontsize=15, weight="bold")
box(ax, 2, 18, 32, 32, "PC", ["192.168.50.1", "ping, curl, browser", "tcp_demo.py", "(dashboard)"], BLUE, fs=9)
box(ax, 48, 18, 34, 32, "Pico Plus  (MASTER)", ["eth0 bridged to radio", "promiscuous, GRO off", "learns remote MAC", "starts with the token"], GREEN, fs=9)
box(ax, 96, 18, 34, 32, "Pico Mini B  (SLAVE)", ["veth pair rf0 <-> veth0", "rf0 = 192.168.50.3", "MTU 236, MSS 184", "L4 checksum fixed"], ORANGE, fs=9)
box(ax, 140, 18, 28, 32, "Linux apps", ["mini_web.py", "HTTP server :80", "SSH, iperf3, ...", "any IP service"], PURPLE, fs=9)
arrow(ax, 34.5, 34, 47.5, 34, "RJ45", both=True)
arrow(ax, 82.5, 34, 95.5, 34, "FLRC", "2600 kbit/s CR 3/4", col=BLUE, both=True, dashed=True)
arrow(ax, 130.5, 34, 139.5, 34, "", both=True)
ax.text(85, 9, "Air frame: [A5][seq][len][flags: TOKEN] + up to 250 B Ethernet frame.  A side sends a burst (max 16), the last frame",
        ha="center", color=SUB, fontsize=8.5)
ax.text(85, 5, "hands over the token. The master takes it back after 60 ms of silence.", ha="center", color=SUB, fontsize=8.5)
save(fig, "tcpip_block_diagram.png")
print("diagrams written to", OUT)
