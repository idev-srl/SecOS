#!/usr/bin/env python3
# tools/gui_wm_drive.py - selftest driver for the secwm compositor + terminal.
# SPDX-License-Identifier: MIT
#
# Boots the SecOS ISO in QEMU (PS/2 keyboard, qemu-xhci + usb-tablet), starts
# the GUI from bash over the serial console and drives it like a user: QMP
# sendkey for the keyboard, input-send-event for the absolute tablet, and
# screendump to look at the screen. secwm --trace mirrors its state changes and
# every line a terminal prints to the console (debugcon log) as SW-* markers;
# the confined isolation probe /bin/swptest prints SWT-* / SWE-* markers.
#   - the guest's debugcon log (--log) carries the in-guest markers;
#   - this driver appends "[WM-DRV] <check> OK|FAIL ..." lines to --out;
#   - --shots DIR keeps the screendumps (PPM, plus PNG when Pillow exists).
# --taskmgr runs the TASKMGR gate instead (U5, /bin/taskmgr in an unprivileged
# user's desktop; --smp N sets the guest CPUs, default 2).
# Never binds host TCP ports (QMP is a private unix socket); only kills the
# QEMU it started.
import argparse, json, os, re, socket, subprocess, sys, tempfile, time

ap = argparse.ArgumentParser()
ap.add_argument("--iso", default="myos.iso")
ap.add_argument("--log", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--shots", default="")
ap.add_argument("--timeout", type=float, default=420)
ap.add_argument("--demo", action="store_true", help="visual tour (screenshots only, no checks)")
ap.add_argument("--taskmgr", action="store_true", help="TASKMGR gate: /bin/taskmgr in a user session")
ap.add_argument("--smp", type=int, default=0, help="guest CPUs (-smp); --taskmgr defaults to 2")
args = ap.parse_args()
if args.taskmgr and not args.smp: args.smp = 2

tmpd = tempfile.mkdtemp(prefix="secos_wm_")
qmp_path = os.path.join(tmpd, "qmp.sock")
cmd = ["qemu-system-x86_64", "-cdrom", args.iso, "-boot", "d", "-m", "512M",
       "-debugcon", "file:" + args.log, "-global", "isa-debugcon.iobase=0xe9",
       "-no-reboot", "-display", "none", "-serial", "stdio",
       "-qmp", "unix:%s,server=on,wait=off" % qmp_path,
       "-device", "qemu-xhci,id=xhci", "-device", "usb-tablet,bus=xhci.0"]
if args.smp: cmd += ["-smp", str(args.smp)]
open(args.log, "w").close()
out = open(args.out, "a")
def note(s):
    out.write("[WM-DRV] " + s + "\n"); out.flush()
    print("[WM-DRV] " + s, flush=True)
def check(name, cond, extra=""):
    note("%s %s %s" % (name, "OK" if cond else "FAIL", extra))
    return cond

q = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
deadline = time.time() + args.timeout

def logtext():
    try:
        with open(args.log, "rb") as f: return f.read().decode("latin-1")
    except OSError: return ""

def wait_for(pat, t=60, after=0):
    end = min(time.time() + t, deadline)
    rx = re.compile(pat)
    while time.time() < end:
        m = rx.search(logtext(), after)
        if m: return m
        if q.poll() is not None: return None
        time.sleep(0.2)
    return None

def type_serial(s):
    q.stdin.write(b"\x15" + s.encode() + b"\r"); q.stdin.flush()

# ---- QMP -----------------------------------------------------------------------
qs = None
for _ in range(100):
    try:
        qs = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); qs.connect(qmp_path); break
    except OSError:
        qs = None; time.sleep(0.1)
if not qs:
    note("qmp-connect FAIL"); q.kill(); sys.exit(1)
qf = qs.makefile("rwb")
def qmp_read():
    while True:
        line = qf.readline()
        if not line: return None
        msg = json.loads(line)
        if "event" in msg: continue
        return msg
qmp_read()
def qmp(name, **a):
    qf.write((json.dumps({"execute": name, "arguments": a}) + "\n").encode()); qf.flush()
    return qmp_read()
qmp("qmp_capabilities")
def hmp(line):
    r = qmp("human-monitor-command", **{"command-line": line})
    return (r or {}).get("return", "")
def sendkey(k, hold=50):
    hmp("sendkey %s %d" % (k, hold)); time.sleep(0.12)

KEYS = {" ": "spc", "-": "minus", "=": "equal", ".": "dot", ",": "comma", "/": "slash",
        ";": "semicolon", "'": "apostrophe", "[": "bracket_left", "]": "bracket_right",
        "\\": "backslash", "`": "grave_accent",
        "!": "shift-1", "@": "shift-2", "#": "shift-3", "$": "shift-4", "%": "shift-5",
        "^": "shift-6", "&": "shift-7", "*": "shift-8", "(": "shift-9", ")": "shift-0",
        "_": "shift-minus", "+": "shift-equal", ":": "shift-semicolon", '"': "shift-apostrophe",
        "<": "shift-comma", ">": "shift-dot", "?": "shift-slash", "|": "shift-backslash",
        "{": "shift-bracket_left", "}": "shift-bracket_right", "~": "shift-grave_accent"}
def type_keys(s, enter=True):
    """Type s on the (US layout) PS/2 keyboard."""
    for ch in s:
        if ch.isalpha() and ch.isupper(): sendkey("shift-" + ch.lower())
        elif ch.isalnum(): sendkey(ch)
        else: sendkey(KEYS[ch])
    if enter: sendkey("ret")

W = H = 0
def to_abs(x, y):
    return int(x * 32767 / max(1, W - 1)), int(y * 32767 / max(1, H - 1))
def move(x, y):
    ax, ay = to_abs(x, y)
    qmp("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": ax}},
                                    {"type": "abs", "data": {"axis": "y", "value": ay}}])
    time.sleep(0.15)
def button(down, b="left"):
    qmp("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": b}}])
    time.sleep(0.15)
def click(x, y):
    move(x, y); button(True); button(False)

shot_n = [0]
def screendump(name):
    shot_n[0] += 1
    path = os.path.join(args.shots or tmpd, "%02d_%s.ppm" % (shot_n[0], name))
    if os.path.exists(path): os.unlink(path)
    qmp("screendump", filename=path)
    for _ in range(50):
        if os.path.exists(path) and os.path.getsize(path) > 100: break
        time.sleep(0.1)
    time.sleep(0.3)
    with open(path, "rb") as f: data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    if args.shots:
        try:
            from PIL import Image
            Image.frombytes("RGB", (w, h), parts[3][:w * h * 3]).save(path[:-4] + ".png")
        except Exception:
            pass
    return w, h, parts[3]
def pixel(img, x, y):
    w, h, d = img
    o = (y * w + x) * 3
    return (d[o], d[o + 1], d[o + 2])
def near(a, b, tol=24):
    return all(abs(int(a[i]) - int(b[i])) <= tol for i in range(3))
def hexrgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)
def mix(a, b, t):
    return tuple(int((a[i] * (255 - t) + b[i] * t) / 255) for i in range(3))
def desk_px(x, y):
    # secwm paint_desktop(): diagonal gradient C_DESK_TOP -> C_DESK_BOT
    t = min(255, y * 200 // H + x * 55 // W)
    return mix(hexrgb(0x0B1622), hexrgb(0x123A44), t)

# ---- boot ------------------------------------------------------------------------
if not wait_for(r"bash-5\.2# ", 120):
    note("boot FAIL (no bash prompt)"); q.kill(); sys.exit(1)
time.sleep(2)

if args.demo:
    # Visual tour for screenshots: terminals running top / vi / less, the clock
    # and the System panel. No pass/fail checks.
    pos = len(logtext())
    type_serial("  /bin/startgui --trace")
    m = wait_for(r"SW-READY (\d+)x(\d+)", 40, pos)
    W, H = int(m.group(1)), int(m.group(2))
    wait_for(r"SW-TERM-OPEN 1 ", 30, pos); time.sleep(2)
    type_keys("ls --color=always /bin | head -c 600; echo; printf '\\033[1;31mred \\033[32mgreen \\033[34mblue\\033[0m\\n'")
    time.sleep(2)
    sendkey("ctrl-alt-t"); time.sleep(3)
    type_keys("top")
    time.sleep(4)
    mb = re.search(r"SW-PANEL System (\d+),(\d+),(\d+),(\d+)", logtext())
    p0 = len(logtext())
    if mb: click(int(mb.group(1)) + 10, int(mb.group(2)) + 10)
    ms = wait_for(r"SW-CLIENT-WINDOW \d+ (\d+)x(\d+) at (-?\d+),(-?\d+)", 30, p0)
    wait_for(r"SYSMON-READY", 30, p0)
    time.sleep(6)
    screendump("demo_sysmon")
    if ms:
        # LVGL tab bar: click "Processes" (middle third of the bar)
        sx, sy, sw = int(ms.group(3)) + 1, int(ms.group(4)) + 26, int(ms.group(1))
        click(sx + sw // 2, sy + 20)
        time.sleep(3)
        screendump("demo_sysmon_procs")
        click(sx + sw // 6, sy + 20)
        time.sleep(2)
    sendkey("ctrl-alt-c"); time.sleep(3)
    move(W // 2 + 40, H // 2 + 60)
    screendump("demo_desktop")
    # vi in a fresh terminal
    sendkey("ctrl-alt-t"); time.sleep(3)
    type_keys("vi /etc/profile")
    time.sleep(3)
    screendump("demo_vi")
    sendkey("esc"); type_keys(":q!"); time.sleep(1)
    type_keys("less /proc/meminfo"); time.sleep(3)
    screendump("demo_less")
    sendkey("q"); time.sleep(1)
    sendkey("ctrl-alt-backspace")
    wait_for(r"SW-EXIT", 15, pos)
    q.kill()
    sys.exit(0)

if args.taskmgr:
    # [U5] TASKMGR gate. The desktop runs as an unprivileged user (tmu, in the
    # video/input groups; secwm --auto switches to it); root keeps a
    # `sleep 2000` on the console. /bin/taskmgr --trace (secwm passes it on)
    # prints TASKMGR-* markers: the row geometry, the dialogs' buttons and every
    # signal it sends. The clicks below use them, like a user would the screen.
    def last_block(after):
        t = logtext()[after:]
        i = t.rfind("TASKMGR-LIST-END")
        if i < 0: return []
        j = t.rfind("TASKMGR-LIST n=", 0, i)
        if j < 0: return []
        return re.findall(r"TASKMGR-P (\d+) (\S+) (\S+) (\S)", t[j:i])
    def rows(after):
        ms = list(re.finditer(r"TASKMGR-ROWS y0=(-?\d+) rh=(\d+) pids=([\d,]*)", logtext()[after:]))
        if not ms: return None
        m = ms[-1]
        return int(m.group(1)), int(m.group(2)), [int(x) for x in m.group(3).split(",") if x]
    def obj(name, after):
        ms = list(re.finditer(r"TASKMGR-OBJ %s (-?\d+),(-?\d+),(\d+),(\d+)" % re.escape(name), logtext()[after:]))
        return tuple(int(v) for v in ms[-1].groups()) if ms else None
    def wclick(o, cx, cy):
        x, y, w, h = o
        click(cx + x + w // 2, cy + y + h // 2)
    def select_row(pid, cx, cy, after, list_obj):
        r = rows(after)
        if not r or pid not in r[2]: return False
        y0, rh, pids = r
        y = y0 + pids.index(pid) * rh + rh // 2
        if list_obj and (y < list_obj[1] + 40 or y > list_obj[1] + list_obj[3] - 4): return False
        p0 = len(logtext())
        click(cx + 60, cy + y)
        return wait_for(r"TASKMGR-SELECT pid=%d " % pid, 8, p0) is not None

    # /proc/net/dev in the Linux layout (the e1000 of the default QEMU NIC).
    pos = len(logtext())
    type_serial("  cat /proc/net/dev; echo NETDEV-END")
    wait_for(r"NETDEV-END", 20, pos)
    t = logtext()[pos:]
    check("procnetdev-format", "Inter-|   Receive" in t and
          " face |bytes    packets errs drop fifo frame compressed multicast|bytes" in t and
          re.search(r"\n\s*eth0:\s*[1-9]\d*\s+[1-9]\d*\s+\d+\s+\d+\s+0\s+0\s+0\s+0\s+[1-9]\d*\s+[1-9]\d*\s+\d+\s+\d+\s+0\s+0\s+0\s+0", t) is not None)
    pos = len(logtext())
    type_serial("  useradd tmu && usermod -aG video,input tmu && printf '1\\nuser=tmu\\n' > /etc/gui-autostart; echo TMSETUP-$?")
    check("tm-setup", wait_for(r"TMSETUP-0", 30, pos) is not None)
    pos = len(logtext())
    type_serial("  sleep 2000 & echo ROOTSLEEP=$!")
    m = wait_for(r"ROOTSLEEP=(\d+)", 15, pos)
    rootpid = int(m.group(1)) if m else -1
    pos = len(logtext())
    type_serial("  /bin/startgui --trace --auto")
    m = wait_for(r"SW-READY (\d+)x(\d+)", 40, pos)
    if not check("tm-wm-ready", m is not None):
        q.kill(); sys.exit(1)
    W, H = int(m.group(1)), int(m.group(2))
    mt = wait_for(r"SW-TERM-OPEN 1 pid=\d+ pts/\d+ at (-?\d+),(-?\d+)", 30, pos)
    tx, ty = (int(mt.group(1)), int(mt.group(2))) if mt else (40, 48)
    time.sleep(2)
    # A sleep owned by the user, started from the GUI terminal.
    p0 = len(logtext())
    click(tx + 60, ty + 200)
    time.sleep(1)
    type_keys("sleep 1000 & echo SLEEP=$! UID=$(id -u)")
    m = wait_for(r"SW-TTY 1: SLEEP=(\d+) UID=(\d+)", 20, p0)
    sleeppid = int(m.group(1)) if m else -1
    check("tm-user-session", m is not None and m.group(2) != "0",
          "sleep=%d uid=%s" % (sleeppid, m.group(2) if m else "?"))
    # The panel entry opens it; Ctrl+Shift+Esc then raises the same instance.
    mb = re.search(r"SW-PANEL TaskManager (\d+),(\d+),(\d+),(\d+)", logtext()[pos:])
    check("tm-panel-button", mb is not None)
    p0 = len(logtext())
    if mb: click(int(mb.group(1)) + int(mb.group(3)) // 2, int(mb.group(2)) + int(mb.group(4)) // 2)
    mw = wait_for(r"SW-CLIENT-WINDOW (\d+) (\d+)x(\d+) at (-?\d+),(-?\d+)", 30, p0)
    mr = wait_for(r"TASKMGR-READY (\d+)x(\d+) cpus=(\d+) user=(\S+) uid=(\d+)", 30, p0)
    check("tm-window-opens", mw is not None and mr is not None)
    if not (mw and mr):
        q.kill(); sys.exit(1)
    wid = mw.group(1)
    cx, cy = int(mw.group(4)) + 1, int(mw.group(5)) + 26
    cw, ch = int(mw.group(2)), int(mw.group(3))
    check("tm-runs-as-user", mr.group(4) == "tmu", "user=%s" % mr.group(4))
    mg = wait_for(r"TASKMGR-CPUGRAPHS (\d+)", 10, p0)
    check("tm-cpus", int(mr.group(3)) == args.smp, "cpus=%s smp=%d" % (mr.group(3), args.smp))
    check("tm-cpu-graphs", mg is not None and int(mg.group(1)) == args.smp,
          "graphs=%s smp=%d" % (mg.group(1) if mg else "?", args.smp))
    time.sleep(3)
    procs = last_block(p0)
    names = set(x[1] for x in procs)
    check("tm-list-init-bash", "init" in names and "bash" in names, "names=%s" % sorted(names))
    check("tm-list-users", any(x[0] == "1" and x[2] == "root" for x in procs) and
          any(int(x[0]) == sleeppid and x[2] == "tmu" for x in procs))
    click(tx + 20, ty + 200)                   # focus the terminal (left of the task manager)
    time.sleep(1)
    p1 = len(logtext())
    sendkey("ctrl-shift-esc")
    check("tm-shortcut-single-instance", wait_for(r"SW-FOCUS %s\b" % wid, 10, p1) is not None and
          re.search(r"SW-CLIENT \d+ pid=\d+ /bin/taskmgr", logtext()[p1:]) is None)
    time.sleep(2)
    img = screendump("taskmgr_processes")
    cols = set(pixel(img, x, y) for y in range(cy, cy + ch, 3) for x in range(cx, cx + cw, 3))
    check("tm-pixels", len(cols) > 40, "colours=%d" % len(cols))
    lst = obj("proc-list", p0)
    # Sort by PID with a click on the header.
    if lst:
        p1 = len(logtext())
        click(cx + 168 + 32, cy + lst[1] + 20)
        check("tm-sort-click", wait_for(r"TASKMGR-SORT processes col=PID desc=0", 8, p1) is not None)
        time.sleep(1.5)
    # End task on the user's sleep: select its row, End task, confirm.
    check("tm-select-row", select_row(sleeppid, cx, cy, p0, lst), "pid=%d" % sleeppid)
    eb = obj("end-task", p0)
    p1 = len(logtext())
    if eb: wclick(eb, cx, cy)
    check("tm-confirm-dialog", wait_for(r"TASKMGR-CONFIRM pid=%d " % sleeppid, 10, p1) is not None)
    time.sleep(1.5)
    screendump("taskmgr_confirm")
    okb = obj("dlg-ok", p1)
    p2 = len(logtext())
    if okb: wclick(okb, cx, cy)
    check("tm-sigterm", wait_for(r"TASKMGR-TERM pid=%d rc=0 " % sleeppid, 10, p2) is not None)
    check("tm-gone", wait_for(r"TASKMGR-GONE pid=%d" % sleeppid, 15, p2) is not None)
    time.sleep(2.5)
    procs = last_block(p2)
    check("tm-gone-from-list", bool(procs) and all(int(x[0]) != sleeppid for x in procs))
    # Ending root's process from this user session is refused; nothing is sent.
    time.sleep(1)
    check("tm-select-root-row", select_row(rootpid, cx, cy, p0, lst), "pid=%d" % rootpid)
    p2 = len(logtext())
    if eb: wclick(eb, cx, cy)
    check("tm-refused", wait_for(r"TASKMGR-REFUSED pid=%d name=sleep owner=root errno=1" % rootpid, 10, p2) is not None)
    time.sleep(1.5)
    screendump("taskmgr_refused")
    check("tm-refused-no-signal", re.search(r"TASKMGR-(CONFIRM|TERM) pid=%d\b" % rootpid, logtext()[p2:]) is None)
    okb = obj("dlg-ok", p2)
    if okb: wclick(okb, cx, cy)
    time.sleep(1)
    # init too, with the keyboard: Home = first row (PID order), Delete = End task.
    p2 = len(logtext())
    sendkey("home"); time.sleep(0.5)
    sendkey("delete")
    check("tm-refused-init-keyboard", wait_for(r"TASKMGR-REFUSED pid=1 name=init owner=root", 10, p2) is not None)
    time.sleep(0.5)
    sendkey("ret")
    time.sleep(1)
    # Performance tab (one graph per CPU), then Users.
    tp = obj("tab-performance", p0)
    p2 = len(logtext())
    if tp: wclick(tp, cx, cy)
    check("tm-tab-performance", wait_for(r"TASKMGR-TAB 1 Performance", 10, p2) is not None)
    time.sleep(3)
    img = screendump("taskmgr_performance")
    pcols = set(pixel(img, x, y) for y in range(cy, cy + ch, 3) for x in range(cx, cx + cw, 3))
    blue = sum(1 for y in range(cy, cy + ch, 2) for x in range(cx + 230, cx + cw, 2)
               if near(pixel(img, x, y), hexrgb(0x3FA4F0), 40))
    check("tm-perf-pixels", len(pcols) > 40 and blue > 20, "colours=%d blue=%d" % (len(pcols), blue))
    check("tm-nic-listed", re.search(r"TASKMGR-NIC eth0", logtext()[p0:]) is not None)
    tu = obj("tab-users", p0)
    p2 = len(logtext())
    if tu: wclick(tu, cx, cy)
    check("tm-tab-users", wait_for(r"TASKMGR-TAB 2 Users", 10, p2) is not None)
    time.sleep(2)
    screendump("taskmgr_users")
    if tp:
        wclick(tp, cx, cy); time.sleep(1)
        # Memory and the NIC in the sidebar (screenshots only).
        click(cx + 110, cy + 40 + 8 + 66 + 30); time.sleep(2.5); screendump("taskmgr_memory")
        click(cx + 110, cy + 40 + 8 + 132 + 30); time.sleep(2.5); screendump("taskmgr_network")
        click(cx + 110, cy + 40 + 8 + 30); time.sleep(1)
    tpr = obj("tab-processes", p0)
    if tpr: wclick(tpr, cx, cy)
    time.sleep(2)
    screendump("taskmgr_final")
    # Ctrl+Alt+W closes it; Ctrl+Shift+Esc opens a new one.
    p2 = len(logtext())
    sendkey("ctrl-alt-w")
    check("tm-close", wait_for(r"SW-CLOSE %s\b" % wid, 10, p2) is not None and
          wait_for(r"SW-CLIENT-EXIT %s " % wid, 10, p2) is not None)
    p2 = len(logtext())
    sendkey("ctrl-shift-esc")
    check("tm-shortcut-open", wait_for(r"TASKMGR-READY ", 30, p2) is not None)
    time.sleep(2)
    p2 = len(logtext())
    sendkey("ctrl-alt-backspace")
    check("tm-exit-gui", wait_for(r"SW-EXIT", 15, p2) is not None)
    time.sleep(2)
    p2 = len(logtext())
    type_serial("  kill -0 %d 2>/dev/null; echo USERSLEEP-$?; kill -0 %d && echo ROOTSLEEP-ALIVE; kill %d"
                % (sleeppid, rootpid, rootpid))
    check("tm-user-sleep-ended", wait_for(r"USERSLEEP-1", 15, p2) is not None)
    check("tm-root-sleep-survived", wait_for(r"ROOTSLEEP-ALIVE", 15, p2) is not None)
    # No capability denial: kill() is CAP_SIGNAL, granted; the refusal is the
    # kernel's credential rule (EPERM), not an [AUDIT] DENY.
    tpids = [int(x) for x in re.findall(r"SW-CLIENT \d+ pid=(\d+) /bin/taskmgr", logtext())]
    denies = re.findall(r"\[AUDIT\] pid=(0x[0-9A-Fa-f]+|\d+) DENY\s+(\S+) \(([^)]*)\)", logtext())
    bad = [d for d in denies if int(d[0], 0) in tpids]
    check("tm-no-audit-deny", bool(tpids) and not bad, "taskmgr pids=%s denies=%s" % (tpids, bad))
    check("tm-no-panic", re.search(r"\[PANIC\]|\[EXC\].*ring0", logtext()) is None)
    q.kill()
    try: q.wait(5)
    except Exception: pass
    sys.exit(0)

# ==== session 1: compositor, terminals, input isolation, job control, layout ====
pos = len(logtext())
type_serial("  /bin/startgui --trace --run /bin/swptest")
m = wait_for(r"SW-READY (\d+)x(\d+)", 40, pos)
if not check("wm-ready", m is not None):
    q.kill(); sys.exit(1)
W, H = int(m.group(1)), int(m.group(2))
mt = wait_for(r"SW-TERM-OPEN 1 pid=\d+ pts/\d+ at (-?\d+),(-?\d+)", 30, pos)
check("term-open", mt is not None)
mc = wait_for(r"SW-CLIENT-WINDOW (\d+) (\d+)x(\d+) at (-?\d+),(-?\d+)", 30, pos)
check("client-window", mc is not None)
check("client-sandbox-fds", wait_for(r"SWT-FDS OK", 5, pos) is not None)
check("client-sandbox-nocap", wait_for(r"SWT-NOCAP OK", 5, pos) is not None)
check("client-sandbox-pts", wait_for(r"SWT-PTS OK", 5, pos) is not None)
check("client-sandbox-confined", wait_for(r"SWT-CONFINED OK", 5, pos) is not None)
time.sleep(3)
img = screendump("desktop")
check("screen-size", img[0] == W and img[1] == H, "%dx%d" % (img[0], img[1]))
# Desktop wallpaper below the panel on the far right (no window opens there),
# the panel at the top, and nothing that looks like the black text console.
dx, dy = W - 6, H // 2
check("desktop-pixel", near(pixel(img, dx, dy), desk_px(dx, dy), 18),
      "got=%s want~%s" % (pixel(img, dx, dy), desk_px(dx, dy)))
check("panel-pixel", near(pixel(img, W // 2, 1), hexrgb(0x18202B), 30), "got=%s" % (pixel(img, W // 2, 1),))
black = sum(1 for y in range(40, H, 16) for x in range(0, W, 16) if pixel(img, x, y) == (0, 0, 0))
check("not-text-console", black < 50, "black=%d" % black)
if mc:
    cx, cy = int(mc.group(4)) + 1 + 120, int(mc.group(5)) + 26 + 100
    check("client-pixels", pixel(img, cx, cy) == (0x12, 0xAB, 0x34), "at %d,%d got=%s" % (cx, cy, pixel(img, cx, cy)))
tx, ty = (int(mt.group(1)), int(mt.group(2))) if mt else (40, 48)

# Focus the terminal with a click on its content and run a command in bash.
pos = len(logtext())
click(tx + 500, ty + 300)
check("term-focus-click", wait_for(r"SW-FOCUS 1\b", 10, pos) is not None)
check("client-focus-out", wait_for(r"SWT-FOCUS 0", 10, pos) is not None)
time.sleep(1)
pos = len(logtext())
type_keys("echo GUI-OK-$((6*7))")
check("pty-echo-42", wait_for(r"SW-TTY 1: GUI-OK-42", 30, pos) is not None)
check("no-key-leak-to-client", re.search(r"SWT-KEY", logtext()[pos:]) is None)

# Job control on the pty: Ctrl+C, Ctrl+Z, fg.
pos = len(logtext())
type_keys("sleep 100")
time.sleep(1.5)
sendkey("ctrl-c")
time.sleep(0.8)
type_keys("echo JC-$?")
check("job-ctrl-c", wait_for(r"SW-TTY 1: JC-130", 20, pos) is not None)
pos = len(logtext())
type_keys("sleep 100")
time.sleep(1.5)
sendkey("ctrl-z")
check("job-ctrl-z", wait_for(r"SW-TTY 1: .*Stopped.*sleep 100", 15, pos) is not None)
time.sleep(0.5)
type_keys("fg")
time.sleep(1.5)
sendkey("ctrl-c")
time.sleep(0.8)
type_keys("echo JF-$?")
check("job-fg", wait_for(r"SW-TTY 1: JF-130", 20, pos) is not None)

# winsize: the pty reports the terminal's grid.
pos = len(logtext())
type_keys("stty size")
check("pty-winsize", wait_for(r"SW-TTY 1: 24 80", 15, pos) is not None)

# Focus the client (Ctrl+Alt+N raises the bottom window, then a click in
# its content): only now does it get keys and pointer buttons.
if mc:
    pos = len(logtext())
    sendkey("ctrl-alt-n")
    check("shortcut-next-window", wait_for(r"SW-FOCUS %s\b" % mc.group(1), 10, pos) is not None)
    check("client-focus-in", wait_for(r"SWT-FOCUS 1", 10, pos) is not None)
    click(int(mc.group(4)) + 60, int(mc.group(5)) + 80)
    sendkey("x")
    check("client-key", wait_for(r"SWT-KEY code=45 cp=120", 10, pos) is not None)
    check("client-button", wait_for(r"SWT-BUTTON \d+,\d+", 5, pos) is not None)
    check("no-key-leak-to-term", re.search(r"SW-TTY 1: .*x", logtext()[pos:]) is None)

# Launcher: the panel "Terminal" button opens a second terminal.
mb = re.search(r"SW-PANEL Terminal (\d+),(\d+),(\d+),(\d+)", logtext())
if mb:
    pos = len(logtext())
    click(int(mb.group(1)) + int(mb.group(3)) // 2, int(mb.group(2)) + int(mb.group(4)) // 2)
    check("launcher-terminal", wait_for(r"SW-TERM-OPEN \d+ pid=", 20, pos) is not None)
else:
    check("launcher-terminal", False, "no SW-PANEL marker")
# Keyboard shortcut: Ctrl+Alt+T opens another one, which gets the focus.
pos = len(logtext())
sendkey("ctrl-alt-t")
m4 = wait_for(r"SW-TERM-OPEN (\d+) pid=", 20, pos)
check("shortcut-terminal", m4 is not None)
time.sleep(2.5)
img = screendump("terminals")

# Italian layout inside the GUI: loadkeys (on the console) is picked up by the
# compositor, then AltGr symbols and the 102nd key reach bash.
if m4:
    tid = m4.group(1)
    pos = len(logtext())
    type_keys("loadkeys it")
    check("layout-it", wait_for(r"SW-LAYOUT it", 15, pos) is not None)
    time.sleep(0.5)
    pos = len(logtext())
    for k in ["e", "c", "h", "o", "spc", "minus", "shift-i", "shift-t", "alt_r-semicolon", "alt_r-apostrophe",
              "less", "shift-less", "alt_r-bracket_left", "alt_r-bracket_right", "shift-alt_r-bracket_left",
              "shift-alt_r-bracket_right", "minus", "ret"]:
        sendkey(k)
    check("layout-it-symbols", wait_for(r"SW-TTY %s: IT@#<>\[\]\{\}" % tid, 15, pos) is not None)
    pos = len(logtext())
    type_keys("loadkeys us")
    check("layout-us", wait_for(r"SW-LAYOUT us", 15, pos) is not None)

# LVGL client: the System launcher starts /bin/sysmon (LVGL 9.2). It must draw
# a real UI (many distinct colours: anti-aliased text, arc, chart), react to a
# click on its "Processes" tab, then close with Ctrl+Alt+W.
mb = re.search(r"SW-PANEL System (\d+),(\d+),(\d+),(\d+)", logtext())
pos = len(logtext())
if mb: click(int(mb.group(1)) + 10, int(mb.group(2)) + 10)
ms = wait_for(r"SW-CLIENT-WINDOW (\d+) (\d+)x(\d+) at (-?\d+),(-?\d+)", 30, pos)
check("lvgl-sysmon-ready", wait_for(r"SYSMON-READY 500x400", 30, pos) is not None and ms is not None)
if ms:
    time.sleep(5)
    sx, sy, sw, sh = int(ms.group(4)) + 1, int(ms.group(5)) + 26, int(ms.group(2)), int(ms.group(3))
    img = screendump("lvgl_overview")
    cols = set(pixel(img, x, y) for y in range(sy, sy + sh, 3) for x in range(sx, sx + sw, 3))
    check("lvgl-sysmon-pixels", len(cols) > 40, "colours=%d" % len(cols))
    area = [(x, y) for y in range(sy + 50, sy + 250, 4) for x in range(sx + 10, sx + sw - 10, 4)]
    click(sx + sw // 2, sy + 20)                       # the "Processes" tab
    time.sleep(3)
    img2 = screendump("lvgl_processes")
    diff = sum(1 for (x, y) in area if pixel(img, x, y) != pixel(img2, x, y))
    check("lvgl-sysmon-click", diff > 300, "changed=%d/%d" % (diff, len(area)))
    p2 = len(logtext())
    sendkey("ctrl-alt-w")
    check("close-shortcut", wait_for(r"SW-CLOSE %s\b" % ms.group(1), 10, p2) is not None)
time.sleep(1)

# Move a window by dragging its title bar.
pos = len(logtext())
move(tx + 150, ty + 12); button(True)
for i in range(1, 6): move(tx + 150 + i * 20, ty + 12 + i * 10)
button(False)
check("drag-move", wait_for(r"SW-MOVED 1 %d,%d" % (tx + 100, ty + 50), 10, pos) is not None)
time.sleep(1)

# Resize it with the grip (bottom-right corner): 80x24 -> 60x18, and the shell
# sees the new size (TIOCSWINSZ -> SIGWINCH -> stty size).
gx, gy = tx + 100 + 650 - 5, ty + 50 + 419 - 5     # outer 650x419 for 80x24
pos = len(logtext())
move(gx, gy); button(True)
for i in range(1, 5): move(gx - i * 40, gy - i * 24)
button(False)
check("resize-grip", wait_for(r"SW-RESIZE 1 60x18", 10, pos) is not None)
time.sleep(1)
type_keys("stty size")
check("resize-winsize", wait_for(r"SW-TTY 1: 18 60", 15, pos) is not None)

# Leave with Ctrl+Alt+Backspace: text console back, bash answers on it.
pos = len(logtext())
sendkey("ctrl-alt-backspace")
check("exit-combo", wait_for(r"SW-EXIT", 15, pos) is not None)
check("exit-message", wait_for(r"secwm: back to the text console", 15, pos) is not None)
time.sleep(2)
img = screendump("text_after_exit")
black = sum(1 for y in range(40, H, 16) for x in range(0, W, 16) if pixel(img, x, y) == (0, 0, 0))
check("text-console-back", black > (W // 16) * ((H - 40) // 16) // 2, "black=%d" % black)
pos = len(logtext())
type_serial("  echo WM-$((6*7))-BACK")
check("bash-after-exit", wait_for(r"WM-42-BACK", 20, pos) is not None)

# ==== session 2: hostile clients ===================================================
pos = len(logtext())
type_serial('  /bin/startgui --trace --no-term --run "/bin/swptest oob" --run "/bin/swptest len" '
            '--run "/bin/swptest read" --run "/bin/swptest early" --run "/bin/swptest big" --run /bin/swptest')
check("wm2-ready", wait_for(r"SW-READY", 40, pos) is not None)
mc = wait_for(r"SW-CLIENT-WINDOW (\d+) 240x140 at (-?\d+),(-?\d+)", 30, pos)
for mode, why in (("oob", "PUT outside the window"), ("len", "PUT length does not match"),
                  ("read", "unknown request type"), ("early", "PUT before CREATE"), ("big", "request too large")):
    check("hostile-" + mode, wait_for(r"SW-VIOLATION client \d+ pid=\d+: %s" % why, 20, pos) is not None)
time.sleep(3)
img = screendump("hostile")
magenta = sum(1 for y in range(0, H, 2) for x in range(0, W, 2) if pixel(img, x, y) == (255, 0, 255))
check("hostile-no-oob-pixels", magenta == 0, "magenta=%d" % magenta)
if mc:
    cx, cy = int(mc.group(2)) + 1 + 120, int(mc.group(3)) + 26 + 100
    check("hostile-good-client-intact", pixel(img, cx, cy) == (0x12, 0xAB, 0x34), "got=%s" % (pixel(img, cx, cy),))
check("hostile-wm-alive", re.search(r"SW-EXIT", logtext()[pos:]) is None)
# Exit with the panel's Exit button this time.
me = re.search(r"SW-PANEL Exit (\d+),(\d+),(\d+),(\d+)", logtext()[pos:])
p2 = len(logtext())
if me:
    click(int(me.group(1)) + 10, int(me.group(2)) + 10)
check("exit-button", wait_for(r"SW-EXIT", 15, p2) is not None)
time.sleep(1)

# ==== session 3: a crashing compositor still gives the console back ===============
pos = len(logtext())
type_serial("  /bin/startgui --trace & sleep 8; kill -SEGV $!; sleep 1; echo WM-CRASH-$((6*7))")
check("crash-ready", wait_for(r"SW-READY", 40, pos) is not None)
check("crash-restored", wait_for(r"\[VT\] text console restored \(graphics owner exited", 30, pos) is not None)
check("crash-bash", wait_for(r"WM-CRASH-42", 30, pos) is not None)
time.sleep(2)
img = screendump("text_after_crash")
black = sum(1 for y in range(40, H, 16) for x in range(0, W, 16) if pixel(img, x, y) == (0, 0, 0))
check("crash-text-console", black > (W // 16) * ((H - 40) // 16) // 2, "black=%d" % black)
pos = len(logtext())
type_serial("  echo WM-$((6*7))-END")
check("bash-responsive", wait_for(r"WM-42-END", 20, pos) is not None)
check("no-panic", re.search(r"\[PANIC\]|\[EXC\].*ring0", logtext()) is None)
q.kill()
try: q.wait(5)
except Exception: pass
