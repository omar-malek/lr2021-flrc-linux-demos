#!/usr/bin/env python3
"""Live FLRC video demo with glass-to-glass latency clock.

Left window : millisecond clock + live link stats (from the receiver log).
Right window: ffplay showing the video received by the Pico Plus over FLRC.
Point the Pico Mini B camera at the clock: latency = clock - clock seen in the video.
Optional: --record out.mp4 records the screen region holding both windows.

Usage: python3 live_demo.py [--seconds 60] [--record demo.mp4] [--gap 1500]
"""
import argparse, subprocess, threading, time, tkinter as tk, signal, os

PLUS, MINI = os.environ.get("PLUS_ADB", ""), os.environ.get("MINI_ADB", "")  # adb serials (adb devices)
ap = argparse.ArgumentParser()
ap.add_argument("--seconds", type=int, default=60)
ap.add_argument("--record", default=None)
ap.add_argument("--gap", type=int, default=1500, help="pause between air packets, us")
ap.add_argument("--rate", type=int, default=2, help="0=2600 1=2080 2=1300 kbit/s raw")
ap.add_argument("--cr", type=int, default=1, help="0=1/2 1=3/4 2=none")
a = ap.parse_args()

procs = []
def run(cmd, **kw):
    p = subprocess.Popen(cmd, **kw); procs.append(p); return p

T = a.seconds + 5
rx = run(["adb", "-s", PLUS, "shell",
          f"stty raw -echo -opost; lr2021_flrc vrx -H - -b 54 -r 55 -R {a.rate} -c {a.cr} -t {T} 2>/tmp/vrx.log"],
         stdout=subprocess.PIPE)
run(["ffplay", "-hide_banner", "-loglevel", "error", "-fflags", "nobuffer", "-flags", "low_delay",
     "-framedrop", "-probesize", "32", "-analyzeduration", "0", "-sync", "ext", "-vf", "setpts=0",
     "-window_title", "FLRC LR2021 DEMO - RX video", "-left", "760", "-top", "60", "-f", "hevc", "-"], stdin=rx.stdout)
time.sleep(2)
run(["adb", "-s", MINI, "shell", f"lr2021_flrc vtx -u /live/1 -b 54 -r 55 -R {a.rate} -c {a.cr} -g {a.gap} -t {a.seconds}"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
if a.record:
    run(["ffmpeg", "-loglevel", "error", "-y", "-f", "x11grab", "-framerate", "30", "-video_size", "1480x700",
         "-i", f"{os.environ.get('DISPLAY', ':0')}+0,40", "-t", str(a.seconds + 3),
         "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p", a.record], stdin=subprocess.DEVNULL)

stats = {"s": "waiting for receiver..."}
def poll():
    while True:
        try:
            out = subprocess.run(["adb", "-s", PLUS, "shell", "grep vrx /tmp/vrx.log | tail -1"],
                                 capture_output=True, text=True, timeout=3).stdout.strip()
            if out: stats["s"] = out
        except Exception:
            pass
        time.sleep(1)
threading.Thread(target=poll, daemon=True).start()

root = tk.Tk(); root.title("FLRC LR2021 DEMO"); root.geometry("740x640+0+40"); root.configure(bg="black")
tk.Label(root, text=f"LR2021 FLRC 2.44 GHz {[2600,2080,1300][a.rate]} kbit/s |  Pico Mini B (camera) -> radio -> Pico Plus -> PC",
         fg="#9cf", bg="black", font=("DejaVu Sans", 12)).pack(pady=6)
tk.Label(root, text="FLRC LR2021 DEMO", fg="#ffffff", bg="#c0392b", font=("DejaVu Sans", 28, "bold")).pack(fill="x", pady=4)
clk = tk.Label(root, fg="white", bg="black", font=("DejaVu Sans Mono", 64, "bold")); clk.pack(pady=30)
tk.Label(root, text="Glass-to-glass latency = this clock - clock seen in the received video",
         fg="#fc6", bg="black", font=("DejaVu Sans", 12)).pack()
st = tk.Label(root, fg="#7f7", bg="black", font=("DejaVu Sans Mono", 11), wraplength=720, justify="left"); st.pack(pady=20)
t0 = time.monotonic()
def tick():
    ms = int((time.monotonic() - t0) * 1000)
    clk.config(text=f"{ms // 1000:4d}.{ms % 1000:03d} s")
    st.config(text=stats["s"].replace("  ", "\n"))
    if time.monotonic() - t0 > a.seconds + 6: root.destroy(); return
    root.after(1, tick)
tick()
try:
    root.mainloop()
finally:
    for p in procs:
        if p.poll() is None: p.send_signal(signal.SIGINT)
    time.sleep(2)
    for p in procs:
        if p.poll() is None: p.kill()
