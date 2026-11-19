#!/usr/bin/env python3
"""FLRC LR2021 DEMO - TCP/IP dashboard.
PC (192.168.50.1) -RJ45- Pico Plus (bridge, master) ~~FLRC 2.4 GHz~~ Pico Mini B (192.168.50.3, rf0)
Shows: live ping RTT graph through the radio, loss, and the radio counters of both boards.
Assumes the 'net' processes already run on both boards (see start_tcp_demo.sh).
"""
import subprocess, threading, time, re, os, tkinter as tk, collections

PLUS, MINI = os.environ.get("PLUS_ADB", ""), os.environ.get("MINI_ADB", "")  # adb serials (adb devices)
MINI_IP = "192.168.50.3"
rtts = collections.deque(maxlen=120); st = {"sent": 0, "ok": 0, "plus": "", "mini": ""}

def pinger():
    while True:
        st["sent"] += 1
        r = subprocess.run(["ping", "-c1", "-W1", "-s", "56", MINI_IP], capture_output=True, text=True)
        m = re.search(r"time=([\d.]+)", r.stdout)
        if m: st["ok"] += 1; rtts.append(float(m.group(1)))
        else: rtts.append(None)
        time.sleep(0.5)

def logs():
    while True:
        for k, s in (("plus", PLUS), ("mini", MINI)):
            try:
                st[k] = subprocess.run(["adb", "-s", s, "shell", "tail -1 /tmp/net.log"], capture_output=True,
                                       text=True, timeout=3).stdout.strip()
            except Exception: pass
        time.sleep(2)

threading.Thread(target=pinger, daemon=True).start(); threading.Thread(target=logs, daemon=True).start()

W, H = 900, 640
root = tk.Tk(); root.title("FLRC LR2021 DEMO - TCP/IP"); root.geometry(f"{W}x{H}+0+40"); root.configure(bg="#0f1720")
tk.Label(root, text="FLRC LR2021 DEMO  -  TCP/IP over 2.4 GHz radio", fg="white", bg="#c0392b",
         font=("DejaVu Sans", 22, "bold")).pack(fill="x")
c = tk.Canvas(root, width=W, height=H - 50, bg="#0f1720", highlightthickness=0); c.pack()

def node(x, t, sub, col):
    c.create_rectangle(x, 15, x + 200, 85, outline=col, width=3, fill="#1b2733")
    c.create_text(x + 100, 37, text=t, fill=col, font=("DejaVu Sans", 13, "bold"))
    c.create_text(x + 100, 63, text=sub, fill="#e6edf3", font=("DejaVu Sans", 10))

def draw():
    c.delete("all")
    node(20, "PC", "192.168.50.1", "#3498db"); node(350, "Pico Plus", "bridge eth0 <-> radio", "#2ecc71")
    node(680, "Pico Mini B", "192.168.50.3 (rf0)", "#f39c12")
    c.create_line(222, 50, 348, 50, fill="white", width=3, arrow="both"); c.create_text(285, 38, text="RJ45", fill="white")
    c.create_line(552, 50, 678, 50, fill="#9cc9ff", width=3, arrow="both", dash=(8, 5))
    c.create_text(615, 30, text="FLRC 2.44 GHz", fill="#9cc9ff"); c.create_text(615, 70, text="2600 kbit/s CR3/4", fill="#9cc9ff")
    # RTT graph
    gx, gy, gw, gh = 60, 130, 800, 250
    c.create_rectangle(gx, gy, gx + gw, gy + gh, outline="#34495e")
    c.create_text(gx, gy - 14, anchor="w", text="Ping PC -> Pico Mini B through the radio (round trip, ms)", fill="white",
                  font=("DejaVu Sans", 12, "bold"))
    vals = [v for v in rtts if v is not None]; top = max(20, max(vals) * 1.2 if vals else 20)
    for k in range(5):
        y = gy + gh - gh * k / 4; c.create_line(gx, y, gx + gw, y, fill="#1f2d3a")
        c.create_text(gx - 8, y, anchor="e", text=f"{top * k / 4:.0f}", fill="#aab7c4")
    pts = []
    for i, v in enumerate(rtts):
        x = gx + gw * i / 119
        if v is None: c.create_line(x, gy, x, gy + gh, fill="#c0392b"); pts.append(None); continue
        pts.append((x, gy + gh - gh * min(v, top) / top))
    seg = []
    for p in pts + [None]:
        if p: seg.append(p)
        else:
            if len(seg) > 1: c.create_line(*[q for pt in seg for q in pt], fill="#2ecc71", width=2)
            seg = []
    last = next((v for v in reversed(rtts) if v is not None), None)
    med = sorted(vals)[len(vals) // 2] if vals else None
    lost = st["sent"] - st["ok"]
    big = [("last", f"{last:.1f} ms" if last else "-"), ("median", f"{med:.1f} ms" if med else "-"),
           ("pings", f"{st['ok']}/{st['sent']}"), ("lost", str(lost))]
    for i, (k, v) in enumerate(big):
        x = 110 + i * 220
        c.create_text(x, 420, text=v, fill="white" if k != "lost" or lost == 0 else "#e74c3c", font=("DejaVu Sans", 22, "bold"))
        c.create_text(x, 452, text=k, fill="#aab7c4", font=("DejaVu Sans", 11))
    c.create_text(20, 500, anchor="w", text="Pico Plus : " + st["plus"], fill="#7f7", font=("DejaVu Sans Mono", 9))
    c.create_text(20, 525, anchor="w", text="Pico Mini : " + st["mini"], fill="#7f7", font=("DejaVu Sans Mono", 9))
    c.create_text(20, 560, anchor="w", text="Browser: http://192.168.50.3  (web page served by the Mini over the radio)",
                  fill="#f1c40f", font=("DejaVu Sans", 11))
    root.after(500, draw)

draw(); root.mainloop()
