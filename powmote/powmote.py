#!/usr/bin/env python3
"""
DC PSU control GUI
Kết nối TCP tới ESP32 (port 5000). Hiển thị telemetry, đặt Vout/Iset,
vẽ biểu đồ 120 s và ghi log lệnh/ACK. Chỉ dùng thư viện chuẩn Python 3.8+.
"""
import socket
import threading
import queue
import time
import collections
import tkinter as tk
from tkinter import messagebox

# ================= CẤU HÌNH =================
HOST_DEFAULT = "192.168.4.1"    # IP AP của ESP32; đổi thành IP STA nếu dùng Wi-Fi nhà
PORT_DEFAULT = 5000
V_MIN, V_MAX = 3.3, 20.5
I_MIN, I_MAX = 0.0, 5.6
HIST_LEN     = 600              # 120 s ở 5 Hz
RECONNECT_S  = 2.0
V_AXIS       = 22.0             # trục biểu đồ điện áp
I_AXIS       = 6.0              # trục biểu đồ dòng
LINK_FRESH_S = 1.0

CMD_NAME = {"1": "Vout", "2": "Iout", "3": "PING"}

# ================= MÀU =================
C_BG      = "#0a0c12"
C_PANEL   = "#141820"
C_HEAD    = "#162a54"
C_TEXT    = "#e6ebf0"
C_GREY    = "#6e7887"
C_LINE    = "#2a303c"
C_GRID    = "#1d2330"
C_VOLT    = "#50e678"
C_AMP     = "#50c8ff"
C_CC      = "#ffa028"
C_WARN    = "#ff4646"


# ================= LIÊN KẾT TCP =================
class Link:
    """Luồng nền: tự kết nối lại, đọc dòng và đẩy vào queue cho GUI."""

    def __init__(self, q):
        self.q = q
        self.gen = 0                 # tăng mỗi lần start/stop để huỷ luồng cũ
        self.sock = None
        self.lock = threading.Lock()
        self.connected = False

    def start(self, host, port):
        self.stop()
        self.gen += 1
        threading.Thread(target=self._run, args=(self.gen, host, port),
                         daemon=True).start()

    def stop(self):
        self.gen += 1
        self._close(notify=True)

    def _close(self, notify=True):
        with self.lock:
            s, self.sock = self.sock, None
        if s:
            try:
                s.close()
            except OSError:
                pass
        if self.connected:
            self.connected = False
            if notify:
                self.q.put(("state", "disconnected"))

    def send(self, line):
        with self.lock:
            s = self.sock
        if s is None:
            return False
        try:
            s.sendall((line + "\n").encode("ascii"))
            return True
        except OSError:
            self._close()
            return False

    def _run(self, gen, host, port):
        last_err = None
        while self.gen == gen:
            try:
                s = socket.create_connection((host, port), timeout=3)
            except OSError as e:
                msg = str(e)
                if msg != last_err:          # không lặp lại cùng một lỗi
                    self.q.put(("error", f"Khong ket noi {host}:{port} - {msg}"))
                    last_err = msg
                time.sleep(RECONNECT_S)
                continue

            last_err = None
            s.settimeout(1.0)
            with self.lock:
                self.sock = s
            self.connected = True
            self.q.put(("state", "connected"))
            self._read(gen, s)
            self._close()
            if self.gen == gen:
                time.sleep(RECONNECT_S)

    def _read(self, gen, s):
        buf = b""
        while self.gen == gen:
            try:
                data = s.recv(1024)
                if not data:
                    break
            except socket.timeout:
                continue
            except OSError:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.q.put(("line", line.decode("ascii", "replace").strip()))


# ================= GIAO DIỆN =================
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("DC PSU Control")
        self.configure(bg=C_BG)
        self.geometry("780x640")
        self.minsize(720, 600)

        self.q = queue.Queue()
        self.link = Link(self.q)
        self.tm = None                       # telemetry mới nhất (dict)
        self.last_rx = 0.0
        self.hist = collections.deque(maxlen=HIST_LEN)

        self._build()
        self.after(50, self._pump)
        self.after(250, self._refresh)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        self.connect()

    # ---------- Xây dựng giao diện ----------
    def _build(self):
        # Thanh kết nối
        top = tk.Frame(self, bg=C_HEAD)
        top.pack(fill="x")

        tk.Label(top, text="Host", bg=C_HEAD, fg=C_TEXT).pack(side="left", padx=(8, 2), pady=6)
        self.host_var = tk.StringVar(value=HOST_DEFAULT)
        tk.Entry(top, textvariable=self.host_var, width=16).pack(side="left", padx=2)

        tk.Label(top, text="Port", bg=C_HEAD, fg=C_TEXT).pack(side="left", padx=(8, 2))
        self.port_var = tk.StringVar(value=str(PORT_DEFAULT))
        tk.Entry(top, textvariable=self.port_var, width=6).pack(side="left", padx=2)

        self.btn_conn = tk.Button(top, text="Kết nối", width=10, command=self.toggle_connect)
        self.btn_conn.pack(side="left", padx=8)

        self.lbl_conn = tk.Label(top, text="Chưa kết nối", bg=C_HEAD, fg=C_GREY)
        self.lbl_conn.pack(side="right", padx=10)

        # Vùng chính
        main = tk.Frame(self, bg=C_BG)
        main.pack(fill="both", expand=True, padx=8, pady=6)

        # --- Hiển thị bên trái ---
        left = tk.Frame(main, bg=C_PANEL, padx=12, pady=8)
        left.pack(side="left", fill="y")

        head = tk.Frame(left, bg=C_PANEL)
        head.pack(fill="x")
        self.lbl_link = tk.Label(head, text="NO LINK", bg=C_PANEL, fg=C_WARN,
                                 font=("Segoe UI", 10, "bold"))
        self.lbl_link.pack(side="left")
        self.lbl_mode = tk.Label(head, text="--", bg=C_GREY, fg=C_BG, width=4,
                                 font=("Segoe UI", 10, "bold"))
        self.lbl_mode.pack(side="right")

        tk.Label(left, text="VOUT", bg=C_PANEL, fg=C_GREY,
                 font=("Segoe UI", 9)).pack(anchor="w", pady=(10, 0))
        self.lbl_v = tk.Label(left, text="--.---", bg=C_PANEL, fg=C_GREY,
                              font=("Consolas", 36, "bold"))
        self.lbl_v.pack(anchor="w")

        tk.Label(left, text="IOUT", bg=C_PANEL, fg=C_GREY,
                 font=("Segoe UI", 9)).pack(anchor="w", pady=(6, 0))
        self.lbl_i = tk.Label(left, text="-.---", bg=C_PANEL, fg=C_GREY,
                              font=("Consolas", 34, "bold"))
        self.lbl_i.pack(anchor="w")

        self.lbl_set = tk.Label(left, text="SET  --", bg=C_PANEL, fg=C_TEXT,
                                font=("Consolas", 10))
        self.lbl_set.pack(anchor="w", pady=(10, 0))
        self.lbl_hw = tk.Label(left, text="ADS --   DAC --", bg=C_PANEL, fg=C_GREY,
                               font=("Consolas", 10))
        self.lbl_hw.pack(anchor="w", pady=(2, 0))

        # --- Điều khiển bên phải ---
        right = tk.Frame(main, bg=C_PANEL, padx=12, pady=8)
        right.pack(side="left", fill="both", expand=True, padx=(8, 0))

        # Vout
        tk.Label(right, text="Điện áp đặt (Vout)", bg=C_PANEL, fg=C_TEXT,
                 font=("Segoe UI", 10, "bold")).pack(anchor="w")
        row_v = tk.Frame(right, bg=C_PANEL)
        row_v.pack(fill="x", pady=(2, 0))
        self.v_var = tk.StringVar(value="3.30")
        ent_v = tk.Entry(row_v, textvariable=self.v_var, width=8, font=("Consolas", 11))
        ent_v.pack(side="left")
        ent_v.bind("<Return>", lambda e: self.set_v())
        tk.Button(row_v, text="Set V", width=7, command=self.set_v).pack(side="left", padx=6)
        tk.Label(row_v, text=f"{V_MIN}–{V_MAX} V", bg=C_PANEL, fg=C_GREY).pack(side="left")

        self.v_scale = tk.Scale(right, from_=V_MIN, to=V_MAX, resolution=0.05,
                                orient="horizontal", showvalue=False, length=300,
                                bg=C_PANEL, fg=C_TEXT, troughcolor=C_LINE,
                                highlightthickness=0, activebackground=C_VOLT,
                                command=lambda s: self.v_var.set(f"{float(s):.2f}"))
        self.v_scale.set(3.3)
        self.v_scale.pack(fill="x", pady=(2, 8))
        self.v_scale.bind("<ButtonRelease-1>", lambda e: self.set_v(self.v_scale.get()))

        # Presets Vout
        pre = tk.Frame(right, bg=C_PANEL)
        pre.pack(fill="x")
        for val in (3.3, 5, 12, 15, 20):
            tk.Button(pre, text=f"{val:g} V", width=6,
                      command=lambda x=val: self.set_v(x)).pack(side="left", padx=2)

        # Iset
        tk.Label(right, text="Giới hạn dòng (Iset)", bg=C_PANEL, fg=C_TEXT,
                 font=("Segoe UI", 10, "bold")).pack(anchor="w", pady=(14, 0))
        row_i = tk.Frame(right, bg=C_PANEL)
        row_i.pack(fill="x", pady=(2, 0))
        self.i_var = tk.StringVar(value="1.00")
        ent_i = tk.Entry(row_i, textvariable=self.i_var, width=8, font=("Consolas", 11))
        ent_i.pack(side="left")
        ent_i.bind("<Return>", lambda e: self.set_i())
        tk.Button(row_i, text="Set I", width=7, command=self.set_i).pack(side="left", padx=6)
        tk.Label(row_i, text=f"{I_MIN}–{I_MAX} A", bg=C_PANEL, fg=C_GREY).pack(side="left")

        self.i_scale = tk.Scale(right, from_=I_MIN, to=I_MAX, resolution=0.01,
                                orient="horizontal", showvalue=False, length=300,
                                bg=C_PANEL, fg=C_TEXT, troughcolor=C_LINE,
                                highlightthickness=0, activebackground=C_AMP,
                                command=lambda s: self.i_var.set(f"{float(s):.2f}"))
        self.i_scale.set(1.0)
        self.i_scale.pack(fill="x", pady=(2, 8))
        self.i_scale.bind("<ButtonRelease-1>", lambda e: self.set_i(self.i_scale.get()))

        # Nút chức năng
        act = tk.Frame(right, bg=C_PANEL)
        act.pack(fill="x", pady=(10, 0))
        tk.Button(act, text="Ping", width=8, command=self.do_ping).pack(side="left", padx=2)
        tk.Button(act, text="Status", width=8, command=self.do_status).pack(side="left", padx=2)
        tk.Button(act, text="Tắt output", width=10, bg="#7a1f1f", fg=C_TEXT,
                  command=self.output_off).pack(side="right", padx=2)

        # --- Biểu đồ ---
        self.chart = tk.Canvas(self, height=190, bg=C_BG, highlightthickness=0)
        self.chart.pack(fill="x", padx=8)
        tk.Label(self, text="Xanh lá: VOUT (V)     Xanh dương: IOUT (A)     cửa sổ 120 s",
                 bg=C_BG, fg=C_GREY).pack(anchor="w", padx=10)

        # --- Log ---
        logf = tk.Frame(self, bg=C_BG)
        logf.pack(fill="both", padx=8, pady=(4, 8))
        sb = tk.Scrollbar(logf)
        sb.pack(side="right", fill="y")
        self.log = tk.Text(logf, height=8, bg=C_PANEL, fg=C_TEXT, state="disabled",
                           yscrollcommand=sb.set, font=("Consolas", 9),
                           relief="flat")
        self.log.pack(fill="both", expand=True)
        sb.config(command=self.log.yview)
        self.log.tag_config("info", foreground=C_TEXT)
        self.log.tag_config("cmd", foreground=C_AMP)
        self.log.tag_config("ok", foreground=C_VOLT)
        self.log.tag_config("warn", foreground=C_CC)
        self.log.tag_config("err", foreground=C_WARN)

    # ---------- Log ----------
    def log_msg(self, text, tag="info"):
        ts = time.strftime("%H:%M:%S")
        self.log.config(state="normal")
        self.log.insert("end", f"{ts}  {text}\n", tag)
        n = int(self.log.index("end-1c").split(".")[0])
        if n > 500:
            self.log.delete("1.0", f"{n - 500}.0")
        self.log.see("end")
        self.log.config(state="disabled")

    # ---------- Kết nối ----------
    def toggle_connect(self):
        if self.link.connected or self.link.gen and self.btn_conn.cget("text") == "Ngắt":
            self.link.stop()
            self.btn_conn.config(text="Kết nối")
            self.log_msg("Đã ngắt kết nối", "info")
        else:
            self.connect()

    def connect(self):
        host = self.host_var.get().strip()
        try:
            port = int(self.port_var.get())
        except ValueError:
            self.log_msg("Port không hợp lệ", "err")
            return
        self.link.start(host, port)
        self.btn_conn.config(text="Ngắt")
        self.log_msg(f"Đang kết nối {host}:{port} ...", "info")

    # ---------- Gửi lệnh ----------
    def _send(self, line):
        if not self.link.send(line):
            self.log_msg("Chưa kết nối, không gửi được", "err")
            return False
        self.log_msg(f"-> {line}", "cmd")
        return True

    def set_v(self, value=None):
        try:
            v = float(self.v_var.get()) if value is None else float(value)
        except ValueError:
            self.log_msg("Vout không hợp lệ", "err")
            return
        if not (V_MIN <= v <= V_MAX):
            self.log_msg(f"Vout ngoài dải {V_MIN}–{V_MAX} V", "err")
            return
        if self._send(f"V {v:.2f}"):
            self.v_var.set(f"{v:.2f}")
            self.v_scale.set(v)

    def set_i(self, value=None):
        try:
            i = float(self.i_var.get()) if value is None else float(value)
        except ValueError:
            self.log_msg("Iset không hợp lệ", "err")
            return
        if not (I_MIN <= i <= I_MAX):
            self.log_msg(f"Iset ngoài dải {I_MIN}–{I_MAX} A", "err")
            return
        if self._send(f"I {i:.2f}"):
            self.i_var.set(f"{i:.2f}")
            self.i_scale.set(i)

    def do_ping(self):
        self._send("PING")

    def do_status(self):
        self._send("STATUS")

    def output_off(self):
        if not messagebox.askyesno("Tắt output", "Đặt Iset = 0 A và Vout = 3.3 V?"):
            return
        self.set_i(0.0)
        self.set_v(3.3)

    # ---------- Nhận dữ liệu ----------
    def _pump(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "line":
                    self._on_line(payload)
                elif kind == "state":
                    if payload == "connected":
                        self.lbl_conn.config(text="Đã kết nối", fg=C_VOLT)
                        self.log_msg("Đã kết nối ESP32", "ok")
                    else:
                        self.lbl_conn.config(text="Mất kết nối", fg=C_WARN)
                        self.log_msg("Mất kết nối, đang thử lại ...", "err")
                elif kind == "error":
                    self.log_msg(payload, "err")
        except queue.Empty:
            pass
        self.after(50, self._pump)

    def _on_line(self, line):
        if not line:
            return

        if line.startswith("T "):
            p = line.split()
            try:
                t = {
                    "vout":   float(p[1]),
                    "iout":   float(p[2]),
                    "vset":   float(p[3]),
                    "iset":   float(p[4]),
                    "mode":   p[5],
                    "status": int(p[6]),
                }
            except (IndexError, ValueError):
                return
            now = time.time()
            self.tm = t
            self.last_rx = now
            self.hist.append((now, t["vout"], t["iout"]))

        elif line.startswith("A "):
            p = line.split()
            cmd = p[1] if len(p) > 1 else "?"
            res = p[2] if len(p) > 2 else "?"
            name = CMD_NAME.get(cmd, cmd)
            tag = "ok" if res == "OK" else ("warn" if res == "CLAMP" else "err")
            self.log_msg(f"ACK {name}: {res}", tag)

        elif line.startswith("ERR"):
            self.log_msg(line, "err")

        elif line.startswith("LINK"):
            self.log_msg(line, "ok" if "UP" in line else "err")

        else:
            self.log_msg(line, "info")

    # ---------- Cập nhật hiển thị ----------
    def _fresh(self):
        return self.tm is not None and (time.time() - self.last_rx) < LINK_FRESH_S

    def _refresh(self):
        fresh = self._fresh()

        if fresh:
            t = self.tm
            self.lbl_link.config(text="LINK OK", fg=C_VOLT)
            self.lbl_v.config(text=f"{t['vout']:6.3f}", fg=C_VOLT)
            self.lbl_i.config(text=f"{t['iout']:6.3f}", fg=C_AMP)

            is_cc = t["mode"] == "CC"
            self.lbl_mode.config(text=t["mode"], bg=C_CC if is_cc else C_VOLT)
            self.lbl_set.config(text=f"SET  {t['vset']:.2f} V   {t['iset']:.2f} A")

            st = t["status"]
            ads_ok = bool(st & 0x01)
            dac_ok = bool(st & 0x02)
            self.lbl_hw.config(
                text=f"ADS {'OK' if ads_ok else 'ERR'}   DAC {'OK' if dac_ok else 'ERR'}",
                fg=C_VOLT if (ads_ok and dac_ok) else C_WARN)
        else:
            self.lbl_link.config(text="NO LINK", fg=C_WARN)
            self.lbl_v.config(text="--.---", fg=C_GREY)
            self.lbl_i.config(text="-.---", fg=C_GREY)
            self.lbl_mode.config(text="--", bg=C_GREY)
            self.lbl_set.config(text="SET  --")
            self.lbl_hw.config(text="ADS --   DAC --", fg=C_GREY)

        self._draw_chart()
        self.after(250, self._refresh)

    def _draw_chart(self):
        c = self.chart
        c.delete("all")
        w = c.winfo_width()
        h = c.winfo_height()
        if w < 20 or h < 20:
            return

        # Lưới và nhãn trục
        for vv in range(0, 23, 5):
            y = h - vv / V_AXIS * h
            c.create_line(0, y, w, y, fill=C_GRID)
            c.create_text(4, y - 2, text=f"{vv} V", anchor="sw",
                          fill="#3f8f5a", font=("Consolas", 8))
        for ii in range(0, 7):
            y = h - ii / I_AXIS * h
            c.create_text(w - 4, y - 2, text=f"{ii} A", anchor="se",
                          fill="#2f6f9a", font=("Consolas", 8))

        pts = list(self.hist)
        n = len(pts)
        if n < 2:
            return

        step = w / HIST_LEN
        vline, iline = [], []
        for k, (_, v, i) in enumerate(pts):
            x = w - (n - 1 - k) * step
            vy = h - min(max(v, 0.0), V_AXIS) / V_AXIS * h
            iy = h - min(max(i, 0.0), I_AXIS) / I_AXIS * h
            vline += [x, vy]
            iline += [x, iy]

        c.create_line(*vline, fill=C_VOLT, width=2)
        c.create_line(*iline, fill=C_AMP, width=2)

    # ---------- Đóng ----------
    def _on_close(self):
        self.link.stop()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()