"""PhoneCam - watch and record an Android "IP Webcam" phone on your PC.

The phone runs the IP Webcam app ("Start server"). This script connects to the
phone over Wi-Fi, shows the live picture in your browser and records it to a
video file on the PC. It can also drive the phone: torch, focus, zoom, front /
back camera, quality, resolution and recording on the phone itself.

    python phonecam.py 192.168.1.23            # phone IP from the IP Webcam screen
    python phonecam.py http://192.168.1.23:8080 --user dad --password secret

Then open http://localhost:8090 (it opens by itself).
"""

import argparse
import base64
import json
import os
import re
import signal
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import cv2
    import numpy as np
except ImportError:
    print("OpenCV is missing. Install it with:  python -m pip install -r requirements.txt")
    sys.exit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "phonecam.json")

SOI = b"\xff\xd8"
EOI = b"\xff\xd9"
CONTENT_LENGTH_RE = re.compile(rb"Content-Length:\s*(\d+)\r?\n(?:[^\r\n]*\r?\n)*?\r?\n", re.IGNORECASE)


def normalize_phone_url(text):
    text = text.strip().rstrip("/")
    if not re.match(r"^https?://", text):
        text = "http://" + text
    parts = urllib.parse.urlsplit(text)
    if parts.port is None:
        # IP Webcam listens on 8080 unless you changed it in the app.
        text = urllib.parse.urlunsplit((parts.scheme, parts.netloc + ":8080", parts.path, "", ""))
    return text


class Phone:
    """Talks to the IP Webcam HTTP server on the phone."""

    def __init__(self, base_url, user=None, password=None):
        self.base_url = base_url
        self.headers = {"User-Agent": "PhoneCam"}
        if user:
            token = base64.b64encode(f"{user}:{password or ''}".encode()).decode()
            self.headers["Authorization"] = "Basic " + token

    def open(self, path, timeout=5):
        req = urllib.request.Request(self.base_url + path, headers=self.headers)
        return urllib.request.urlopen(req, timeout=timeout)

    def get(self, path, timeout=5):
        with self.open(path, timeout) as resp:
            return resp.read()


class CameraStream:
    """Reads the phone's MJPEG stream in the background and keeps the latest frame."""

    def __init__(self, phone):
        self.phone = phone
        self.cond = threading.Condition()
        self.jpeg = None
        self.frame_id = 0
        self.size = None
        self.connected = False
        self.error = "connecting..."
        self.times = deque(maxlen=30)
        self.running = True
        threading.Thread(target=self._run, name="camera", daemon=True).start()

    @property
    def fps(self):
        with self.cond:
            if len(self.times) < 2 or self.times[-1] == self.times[0]:
                return 0.0
            return (len(self.times) - 1) / (self.times[-1] - self.times[0])

    def latest(self):
        with self.cond:
            return self.jpeg, self.frame_id

    def wait_frame(self, last_id, timeout=5):
        with self.cond:
            self.cond.wait_for(lambda: self.frame_id != last_id, timeout)
            return self.jpeg, self.frame_id

    def _publish(self, jpeg):
        if self.size is None or self.frame_id % 30 == 0:
            # Check the picture size now and then (the phone's resolution can change).
            img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_GRAYSCALE)
            if img is not None:
                self.size = (img.shape[1], img.shape[0])
        with self.cond:
            self.jpeg = jpeg
            self.frame_id += 1
            self.times.append(time.monotonic())
            self.cond.notify_all()

    def _run(self):
        while self.running:
            try:
                with self.phone.open("/video", timeout=5) as resp:
                    self.connected = True
                    self.error = None
                    self._read_stream(resp)
                self.error = "stream ended"
            except urllib.error.HTTPError as e:
                self.error = "wrong user / password" if e.code == 401 else f"HTTP {e.code}"
            except Exception as e:
                self.error = str(getattr(e, "reason", e)) or e.__class__.__name__
            self.connected = False
            with self.cond:
                self.times.clear()
            time.sleep(2)

    def _read_stream(self, resp):
        buf = b""
        while self.running:
            chunk = resp.read1(65536)
            if not chunk:
                return
            buf += chunk
            while True:
                # Prefer the part's Content-Length; fall back to JPEG start/end markers.
                m = CONTENT_LENGTH_RE.search(buf)
                if m:
                    start, length = m.end(), int(m.group(1))
                    if len(buf) < start + length:
                        break
                    frame = buf[start:start + length]
                    buf = buf[start + length:]
                    if frame.startswith(SOI):
                        self._publish(frame)
                    continue
                s = buf.find(SOI)
                if s < 0:
                    buf = buf[-4096:]
                    break
                e = buf.find(EOI, s + 2)
                if e < 0:
                    buf = buf[s:]
                    break
                self._publish(buf[s:e + 2])
                buf = buf[e + 2:]
            if len(buf) > 16 * 1024 * 1024:
                buf = b""


class Recorder:
    """Writes the live picture to a video file on the PC at a steady frame rate."""

    def __init__(self, camera, out_dir, fps=None):
        self.camera = camera
        self.out_dir = out_dir
        self.fps_override = fps
        self.lock = threading.Lock()
        self.thread = None
        self.stop_event = threading.Event()
        self.path = None
        self.started = None
        self.frames = 0
        self.error = None

    @property
    def recording(self):
        return self.thread is not None and self.thread.is_alive()

    def start(self):
        with self.lock:
            if self.recording:
                return self.path
            jpeg, _ = self.camera.latest()
            if not self.camera.connected or jpeg is None:
                raise RuntimeError("no picture from the phone yet")
            fps = self.fps_override or min(60, max(5, round(self.camera.fps or 15)))
            os.makedirs(self.out_dir, exist_ok=True)
            name = "phonecam_" + datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
            writer, self.path = self._open_writer(name, jpeg, fps)
            self.stop_event.clear()
            self.started = time.time()
            self.frames = 0
            self.error = None
            self.thread = threading.Thread(target=self._run, args=(writer, fps), name="recorder", daemon=True)
            self.thread.start()
            return self.path

    def stop(self):
        with self.lock:
            thread = self.thread
            if thread is None:
                return None
            self.stop_event.set()
            thread.join(10)
            self.thread = None
            return self.path

    def _open_writer(self, name, jpeg, fps):
        img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
        if img is None:
            raise RuntimeError("could not decode the picture from the phone")
        size = (img.shape[1], img.shape[0])
        for ext, codec in ((".mp4", "mp4v"), (".avi", "MJPG")):
            path = os.path.join(self.out_dir, name + ext)
            writer = cv2.VideoWriter(path, cv2.VideoWriter_fourcc(*codec), fps, size)
            if writer.isOpened():
                return writer, path
            writer.release()
        raise RuntimeError("could not create a video file in " + self.out_dir)

    def _run(self, writer, fps):
        size = None
        img, img_id = None, -1
        t0 = time.monotonic()
        try:
            while not self.stop_event.is_set():
                jpeg, frame_id = self.camera.latest()
                if frame_id != img_id and jpeg is not None:
                    decoded = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
                    if decoded is not None:
                        size = size or (decoded.shape[1], decoded.shape[0])
                        if (decoded.shape[1], decoded.shape[0]) != size:
                            decoded = cv2.resize(decoded, size)
                        img, img_id = decoded, frame_id
                # Write one frame per tick; repeat the last picture when the phone is
                # slower, so the video always plays back at real-time speed.
                due = int((time.monotonic() - t0) * fps) + 1
                while self.frames < due and img is not None:
                    writer.write(img)
                    self.frames += 1
                self.stop_event.wait(max(0.0, t0 + self.frames / fps - time.monotonic()))
        except Exception as e:
            self.error = str(e)
        finally:
            writer.release()


PHONE_COMMANDS = {
    "torch_on": "/enabletorch",
    "torch_off": "/disabletorch",
    "focus": "/focus",
    "focus_off": "/nofocus",
    "front_camera": "/settings/ffc?set=on",
    "back_camera": "/settings/ffc?set=off",
    "phone_record_stop": "/stopvideo?force=1",
}


class App:
    def __init__(self, phone, camera, recorder, out_dir):
        self.phone = phone
        self.camera = camera
        self.recorder = recorder
        self.out_dir = out_dir
        self.phone_status = {}
        self.phone_status_time = 0
        self.phone_status_lock = threading.Lock()

    def status(self):
        rec = self.recorder
        return {
            "phone": self.phone.base_url,
            "connected": self.camera.connected,
            "error": self.camera.error,
            "fps": round(self.camera.fps, 1),
            "size": self.camera.size,
            "recording": rec.recording,
            "recording_file": os.path.basename(rec.path) if rec.path else None,
            "recording_seconds": round(time.time() - rec.started) if rec.recording else 0,
            "recording_error": rec.error,
        }

    def get_phone_status(self):
        # Cached so several open browser tabs do not hammer the phone.
        with self.phone_status_lock:
            if time.time() - self.phone_status_time > 3:
                try:
                    data = json.loads(self.phone.get("/status.json?show_avail=1", timeout=3))
                    self.phone_status = {
                        "curvals": data.get("curvals", {}),
                        "video_sizes": data.get("avail", {}).get("video_size", []),
                    }
                except Exception as e:
                    self.phone_status = {"error": str(e)}
                self.phone_status_time = time.time()
            return self.phone_status

    def phone_command(self, cmd, value):
        if cmd in PHONE_COMMANDS:
            path = PHONE_COMMANDS[cmd]
        elif cmd == "phone_record_start":
            path = "/startvideo?force=1&tag=phonecam_" + datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        elif cmd == "quality" and value.isdigit() and 1 <= int(value) <= 100:
            path = "/settings/quality?set=" + value
        elif cmd == "zoom" and value.isdigit() and 0 <= int(value) <= 100:
            path = "/ptz?zoom=" + value
        elif cmd == "video_size" and re.fullmatch(r"\d{2,5}x\d{2,5}", value):
            path = "/settings/video_size?set=" + value
        else:
            raise ValueError("unknown command")
        self.phone.get(path, timeout=5)
        self.phone_status_time = 0
        return path

    def snapshot(self):
        jpeg, _ = self.camera.latest()
        if jpeg is None:
            raise RuntimeError("no picture from the phone yet")
        os.makedirs(self.out_dir, exist_ok=True)
        name = "phonecam_" + datetime.now().strftime("%Y-%m-%d_%H-%M-%S_%f")[:-3] + ".jpg"
        with open(os.path.join(self.out_dir, name), "wb") as f:
            f.write(jpeg)
        return name

    def recordings(self):
        if not os.path.isdir(self.out_dir):
            return []
        items = []
        for name in os.listdir(self.out_dir):
            path = os.path.join(self.out_dir, name)
            if os.path.isfile(path) and name.lower().endswith((".mp4", ".avi", ".jpg")):
                st = os.stat(path)
                items.append({"name": name, "size": st.st_size, "time": st.st_mtime})
        items.sort(key=lambda i: i["time"], reverse=True)
        return items[:50]


def make_handler(app):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        def send_json(self, data, code=200):
            body = json.dumps(data).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            if url.path == "/":
                body = PAGE.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif url.path == "/stream.mjpg":
                self.stream()
            elif url.path == "/api/status":
                self.send_json(app.status())
            elif url.path == "/api/phone/status":
                self.send_json(app.get_phone_status())
            elif url.path == "/api/recordings":
                self.send_json(app.recordings())
            elif url.path.startswith("/recordings/"):
                self.send_recording(urllib.parse.unquote(url.path[len("/recordings/"):]))
            else:
                self.send_json({"error": "not found"}, 404)

        def do_POST(self):
            url = urllib.parse.urlsplit(self.path)
            query = dict(urllib.parse.parse_qsl(url.query))
            self.rfile.read(int(self.headers.get("Content-Length") or 0))
            try:
                if url.path == "/api/record/start":
                    self.send_json({"file": os.path.basename(app.recorder.start())})
                elif url.path == "/api/record/stop":
                    path = app.recorder.stop()
                    self.send_json({"file": os.path.basename(path) if path else None})
                elif url.path == "/api/snapshot":
                    self.send_json({"file": app.snapshot()})
                elif url.path == "/api/phone":
                    self.send_json({"sent": app.phone_command(query.get("cmd", ""), query.get("value", ""))})
                else:
                    self.send_json({"error": "not found"}, 404)
            except Exception as e:
                self.send_json({"error": str(getattr(e, "reason", e))}, 400)

        def stream(self):
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            last = -1
            try:
                while True:
                    jpeg, last_new = app.camera.wait_frame(last, timeout=5)
                    if jpeg is None or last_new == last:
                        continue
                    last = last_new
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(jpeg))
                    self.wfile.write(jpeg)
                    self.wfile.write(b"\r\n")
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass
            self.close_connection = True

        def send_recording(self, name):
            path = os.path.join(app.out_dir, os.path.basename(name))
            if os.path.basename(name) != name or not os.path.isfile(path):
                self.send_json({"error": "not found"}, 404)
                return
            kind = {".mp4": "video/mp4", ".avi": "video/x-msvideo", ".jpg": "image/jpeg"}.get(
                os.path.splitext(name)[1].lower(), "application/octet-stream")
            self.send_response(200)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(os.path.getsize(path)))
            self.send_header("Content-Disposition", f'attachment; filename="{name}"')
            self.end_headers()
            try:
                with open(path, "rb") as f:
                    while chunk := f.read(1024 * 1024):
                        self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

    return Handler


PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PhoneCam</title>
<style>
  :root { --bg:#111418; --panel:#1b2027; --line:#2c333d; --text:#e8ecf1; --dim:#8b96a5; --red:#e5484d; --green:#3dd68c; --accent:#4c8dff; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--text); font:14px/1.4 system-ui, Segoe UI, sans-serif; }
  header { display:flex; align-items:center; gap:12px; padding:10px 16px; border-bottom:1px solid var(--line); }
  header h1 { font-size:16px; margin:0; }
  .dot { width:10px; height:10px; border-radius:50%; background:var(--dim); display:inline-block; }
  .dot.on { background:var(--green); }
  #info { color:var(--dim); }
  main { display:grid; grid-template-columns: 1fr 300px; gap:16px; padding:16px; }
  @media (max-width: 900px) { main { grid-template-columns: 1fr; } }
  .view { position:relative; background:#000; border-radius:8px; overflow:hidden; min-height:240px; display:flex; align-items:center; justify-content:center; }
  .view img { width:100%; height:auto; max-height:calc(100vh - 110px); object-fit:contain; display:block; }
  .rec-badge { position:absolute; top:12px; left:12px; background:var(--red); color:#fff; padding:4px 10px; border-radius:4px; font-weight:600; display:none; }
  .rec-badge.on { display:block; animation: blink 1.2s infinite; }
  @keyframes blink { 50% { opacity:.55; } }
  #msg { position:absolute; color:var(--dim); }
  aside { display:flex; flex-direction:column; gap:12px; }
  section { background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:12px; }
  section h2 { font-size:12px; text-transform:uppercase; letter-spacing:.06em; color:var(--dim); margin:0 0 10px; }
  .row { display:flex; gap:8px; flex-wrap:wrap; margin-bottom:8px; }
  .row:last-child { margin-bottom:0; }
  button, select { background:#252b34; color:var(--text); border:1px solid var(--line); border-radius:6px; padding:8px 12px; font:inherit; cursor:pointer; }
  button:hover { border-color:var(--accent); }
  button.big { width:100%; padding:12px; font-size:15px; font-weight:600; }
  button.rec { background:var(--red); border-color:var(--red); color:#fff; }
  button.rec.stop { background:#252b34; color:var(--red); }
  label { color:var(--dim); display:flex; align-items:center; gap:8px; width:100%; }
  input[type=range] { flex:1; }
  ul { list-style:none; margin:0; padding:0; max-height:220px; overflow:auto; }
  li { display:flex; justify-content:space-between; gap:8px; padding:4px 0; border-bottom:1px solid var(--line); }
  li a { color:var(--accent); text-decoration:none; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  li span { color:var(--dim); flex:none; }
  #toast { position:fixed; bottom:16px; left:50%; transform:translateX(-50%); background:#2a313b; border:1px solid var(--line); padding:8px 14px; border-radius:6px; opacity:0; transition:opacity .2s; }
  #toast.show { opacity:1; }
</style>
</head>
<body>
<header>
  <span class="dot" id="dot"></span>
  <h1>PhoneCam</h1>
  <span id="info">connecting...</span>
</header>
<main>
  <div class="view">
    <span id="msg">Waiting for the phone...</span>
    <img id="live" src="/stream.mjpg" alt="">
    <div class="rec-badge" id="badge">REC 00:00</div>
  </div>
  <aside>
    <section>
      <h2>Record on this PC</h2>
      <div class="row"><button class="big rec" id="recBtn">Start recording</button></div>
      <div class="row"><button class="big" id="snapBtn">Take snapshot</button></div>
    </section>
    <section>
      <h2>Phone camera</h2>
      <div class="row">
        <button data-cmd="torch_on">Torch on</button>
        <button data-cmd="torch_off">Torch off</button>
        <button data-cmd="focus">Focus</button>
      </div>
      <div class="row">
        <button data-cmd="back_camera">Back camera</button>
        <button data-cmd="front_camera">Front camera</button>
      </div>
      <div class="row"><label>Zoom <input type="range" id="zoom" min="0" max="100" value="0"></label></div>
      <div class="row"><label>Quality <input type="range" id="quality" min="10" max="100" value="50"></label></div>
      <div class="row"><label>Resolution <select id="size"><option value="">-</option></select></label></div>
    </section>
    <section>
      <h2>Record on the phone</h2>
      <div class="row">
        <button data-cmd="phone_record_start">Start</button>
        <button data-cmd="phone_record_stop">Stop</button>
      </div>
    </section>
    <section>
      <h2>Saved on this PC</h2>
      <ul id="files"><li><span>nothing yet</span></li></ul>
    </section>
  </aside>
</main>
<div id="toast"></div>
<script>
const $ = id => document.getElementById(id);
let recording = false, toastTimer;

function toast(text) {
  $("toast").textContent = text; $("toast").classList.add("show");
  clearTimeout(toastTimer); toastTimer = setTimeout(() => $("toast").classList.remove("show"), 2500);
}
async function post(url) {
  const r = await fetch(url, { method: "POST" });
  const data = await r.json();
  if (!r.ok) throw new Error(data.error || r.statusText);
  return data;
}
function phone(cmd, value = "") {
  return post(`/api/phone?cmd=${cmd}&value=${encodeURIComponent(value)}`)
    .then(() => toast("Sent to phone"), e => toast("Phone: " + e.message));
}
const fmtTime = s => String(Math.floor(s / 60)).padStart(2, "0") + ":" + String(s % 60).padStart(2, "0");
const fmtSize = b => b > 1e9 ? (b / 1e9).toFixed(1) + " GB" : b > 1e6 ? (b / 1e6).toFixed(1) + " MB" : Math.ceil(b / 1e3) + " KB";

async function refresh() {
  try {
    const s = await (await fetch("/api/status")).json();
    $("dot").classList.toggle("on", s.connected);
    $("msg").textContent = s.connected ? "" : "Waiting for the phone... " + (s.error || "");
    $("info").textContent = s.connected
      ? `${s.phone}  -  ${s.size ? s.size.join("x") : ""}  ${s.fps} fps`
      : `${s.phone}  -  not connected (${s.error || "..."})`;
    recording = s.recording;
    $("recBtn").textContent = recording ? "Stop recording" : "Start recording";
    $("recBtn").classList.toggle("stop", recording);
    $("badge").classList.toggle("on", recording);
    $("badge").textContent = "REC " + fmtTime(s.recording_seconds);
    if (s.recording_error) toast("Recording error: " + s.recording_error);
  } catch (e) { $("info").textContent = "PhoneCam is not running"; }
}
async function refreshFiles() {
  const files = await (await fetch("/api/recordings")).json();
  $("files").innerHTML = files.length ? "" : "<li><span>nothing yet</span></li>";
  for (const f of files) {
    const li = document.createElement("li");
    const a = document.createElement("a");
    a.href = "/recordings/" + encodeURIComponent(f.name); a.textContent = f.name;
    const size = document.createElement("span"); size.textContent = fmtSize(f.size);
    li.append(a, size); $("files").append(li);
  }
}
async function refreshPhone() {
  const p = await (await fetch("/api/phone/status")).json();
  if (!p.curvals) return;
  const c = p.curvals, sel = $("size");
  if (p.video_sizes && p.video_sizes.length && sel.options.length <= 1) {
    sel.innerHTML = p.video_sizes.map(v => `<option>${v}</option>`).join("");
  }
  if (c.video_size && document.activeElement !== sel) sel.value = c.video_size;
  if (c.quality && document.activeElement !== $("quality")) $("quality").value = c.quality;
}

$("recBtn").onclick = async () => {
  try {
    const r = await post(recording ? "/api/record/stop" : "/api/record/start");
    toast((recording ? "Saved " : "Recording to ") + r.file);
  } catch (e) { toast(e.message); }
  refresh(); refreshFiles();
};
$("snapBtn").onclick = () => post("/api/snapshot").then(r => { toast("Saved " + r.file); refreshFiles(); }, e => toast(e.message));
document.querySelectorAll("[data-cmd]").forEach(b => b.onclick = () => phone(b.dataset.cmd));
$("zoom").onchange = e => phone("zoom", e.target.value);
$("quality").onchange = e => phone("quality", e.target.value);
$("size").onchange = e => e.target.value && phone("video_size", e.target.value);

// Reload the picture if the stream drops (phone asleep, Wi-Fi hiccup).
$("live").onerror = () => setTimeout(() => $("live").src = "/stream.mjpg?" + Date.now(), 2000);

refresh(); refreshFiles(); refreshPhone();
setInterval(refresh, 1000);
setInterval(refreshFiles, 5000);
setInterval(refreshPhone, 5000);
</script>
</body>
</html>
"""


def load_config():
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_config(cfg):
    try:
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
    except OSError:
        pass


def main():
    parser = argparse.ArgumentParser(description="Watch and record an IP Webcam phone on this PC.")
    parser.add_argument("phone", nargs="?", help="phone address shown in IP Webcam, e.g. 192.168.1.23:8080")
    parser.add_argument("--user", help="IP Webcam login (if you set one in the app)")
    parser.add_argument("--password", help="IP Webcam password")
    parser.add_argument("--port", type=int, default=8090, help="port of this PC's viewer page (default 8090)")
    parser.add_argument("--host", default="127.0.0.1",
                        help="0.0.0.0 to open the viewer from other devices on the Wi-Fi (default: this PC only)")
    parser.add_argument("--out", default=os.path.join(HERE, "recordings"), help="folder for videos and snapshots")
    parser.add_argument("--fps", type=int, help="frame rate of recordings (default: what the phone sends)")
    parser.add_argument("--no-browser", action="store_true", help="do not open the browser")
    args = parser.parse_args()

    cfg = load_config()
    address = args.phone or cfg.get("phone")
    if not address:
        print("Open IP Webcam on the phone, tap 'Start server' and look at the bottom of the")
        print("screen for an address like  http://192.168.1.23:8080")
        address = input("Phone address: ").strip()
        if not address:
            sys.exit("No phone address given.")
    user = args.user if args.user is not None else cfg.get("user")
    password = args.password if args.password is not None else cfg.get("password")
    save_config({"phone": address, "user": user, "password": password})

    phone = Phone(normalize_phone_url(address), user, password)
    camera = CameraStream(phone)
    out_dir = os.path.abspath(args.out)
    recorder = Recorder(camera, out_dir, args.fps)
    app = App(phone, camera, recorder, out_dir)

    server = ThreadingHTTPServer((args.host, args.port), make_handler(app))
    server.daemon_threads = True
    url = f"http://localhost:{args.port}"
    print(f"Phone:      {phone.base_url}")
    print(f"Viewer:     {url}")
    print(f"Recordings: {out_dir}")
    print("Press Ctrl+C to quit.")

    def shutdown(*_):
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)
    if not args.no_browser:
        threading.Timer(0.8, webbrowser.open, args=(url,)).start()
    try:
        server.serve_forever()
    finally:
        # Finish the video file so it plays, even when closed in the middle of a recording.
        path = recorder.stop()
        camera.running = False
        if path:
            print("Saved", path)
        server.server_close()


if __name__ == "__main__":
    main()
