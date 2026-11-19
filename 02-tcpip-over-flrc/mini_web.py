#!/usr/bin/env python3
"""Tiny web server for the Pico Mini B: served to the PC THROUGH the FLRC radio link.
GET /          -> demo page (auto-refreshing board info + photo loaded over the radio)
GET /info      -> JSON: uptime, load, CPU temperature, rf0 counters
GET /photo.jpg -> /tmp/photo.jpg (pushed by the PC)
"""
import http.server, json, os, time

PAGE = b"""<!doctype html><html><head><meta charset=utf-8><title>Pico Mini B over FLRC</title>
<style>body{background:#0f1720;color:#e6edf3;font-family:sans-serif;margin:20px}
h1{background:#c0392b;padding:8px 14px;font-size:26px}.k{color:#9cc9ff}
#i{font:16px monospace;line-height:1.6}img{max-width:100%;border:2px solid #f39c12}</style></head><body>
<h1>FLRC LR2021 DEMO &mdash; web page served by the Pico Mini B over the 2.4 GHz FLRC radio</h1>
<div id=i>loading...</div><p class=k>Photo transferred over the radio (TCP/HTTP):</p>
<img src="/photo.jpg?t=START">
<script>t0=performance.now();document.querySelector('img').src='/photo.jpg?t='+Date.now();
document.querySelector('img').onload=()=>{document.querySelector('.k').textContent=
'Photo transferred over the radio (TCP/HTTP): loaded in '+((performance.now()-t0)/1000).toFixed(1)+' s'};
async function f(){try{const a=performance.now();const r=await (await fetch('/info')).json();
const ms=(performance.now()-a).toFixed(0);
document.getElementById('i').innerHTML='Board: Luckfox Pico Mini B &nbsp; IP 192.168.50.3 (rf0)<br>'+
'Uptime: '+r.uptime+' s &nbsp; Load: '+r.load+' &nbsp; CPU temp: '+r.temp+' &deg;C<br>'+
'rf0 RX '+r.rx+' B &nbsp; TX '+r.tx+' B<br>HTTP round trip over radio: <b>'+ms+' ms</b>';}catch(e){}
setTimeout(f,1000)}f();</script></body></html>"""

def rd(p, d="?"):
    try: return open(p).read().strip()
    except Exception: return d

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, body, ctype):
        self.send_response(200); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.send_header("Cache-Control", "no-store")
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        if self.path.startswith("/info"):
            t = rd("/sys/class/thermal/thermal_zone0/temp", "0")
            self.send(json.dumps({"uptime": rd("/proc/uptime").split()[0], "load": rd("/proc/loadavg").split()[0],
                "temp": round(int(t) / 1000, 1) if t.isdigit() else "?",
                "rx": rd("/sys/class/net/rf0/statistics/rx_bytes"), "tx": rd("/sys/class/net/rf0/statistics/tx_bytes")}).encode(),
                "application/json")
        elif self.path.startswith("/photo.jpg"):
            self.send(open("/tmp/photo.jpg", "rb").read() if os.path.exists("/tmp/photo.jpg") else b"", "image/jpeg")
        else:
            self.send(PAGE, "text/html")

http.server.ThreadingHTTPServer(("192.168.50.3", 80), H).serve_forever()
