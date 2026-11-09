#!/usr/bin/env python3
# tools/nano_drive.py - NANO gate driver (U8): GNU nano on SecOS, driven like a user.
# SPDX-License-Identifier: MIT
#
# One QEMU boot (ISO built CONSOLE=root, no root disk) with a user-net e1000
# (host port --port -> guest :22), a PS/2 keyboard, a qemu-xhci + usb-tablet
# and a private QMP socket. Three sessions edit a file the same way: open it,
# type UTF-8 text (à è), search, cut a line and paste it elsewhere, save,
# exit; the saved bytes are then read back with od and compared:
#   ssh  Dropbear session on a host pty (the tools/ssh_winch.py approach),
#        TERM=xterm-256color, 30x90; the host pty is resized to 40x100 while
#        nano runs (SSH window-change -> pty TIOCSWINSZ -> SIGWINCH) and nano
#        must repaint for 40 rows;
#   con  the kernel console (serial in, fb console + serial out), TERM=linux;
#   gui  a secwm terminal (TERM=xterm), typed with QMP sendkey; à è come from
#        the Italian layout (loadkeys it, picked up live by secwm); the window
#        is shrunk with the resize grip while nano runs and nano must repaint
#        its help bar for the new 18 rows (checked on a screendump).
# Every check is a line "[NANO-DRV] <name> OK|FAIL ..." on stdout and in --out.
# Never binds host ports other than --port (not 2222/8000); kills only the
# QEMU it started.
import argparse, fcntl, json, os, pty, re, select, shutil, signal, socket, struct
import subprocess, sys, tempfile, termios, time

ap = argparse.ArgumentParser()
ap.add_argument("--iso", default="myos.iso")
ap.add_argument("--log", required=True, help="guest debugcon log")
ap.add_argument("--out", required=True)
ap.add_argument("--port", type=int, required=True, help="host port forwarded to guest :22")
ap.add_argument("--smp", type=int, default=2)
ap.add_argument("--timeout", type=float, default=600)
ap.add_argument("--shots", default="")
ap.add_argument("--only", default="ssh,con,gui")
args = ap.parse_args()
if args.port in (2222, 8000):
    sys.exit("nano_drive: refusing host port %d" % args.port)
modes = set(args.only.split(","))

tmpd = tempfile.mkdtemp(prefix="secos_nano_")
qmp_path = os.path.join(tmpd, "qmp.sock")
out = open(args.out, "a")
fails = [0]

def note(s):
    out.write("[NANO-DRV] " + s + "\n"); out.flush()
    print("[NANO-DRV] " + s, flush=True)

def check(name, cond, extra=""):
    note("%s %s %s" % (name, "OK" if cond else "FAIL", extra))
    if not cond: fails[0] += 1
    return cond

cmd = ["qemu-system-x86_64", "-cdrom", args.iso, "-boot", "d", "-m", "512M", "-smp", str(args.smp),
       "-debugcon", "file:" + args.log, "-global", "isa-debugcon.iobase=0xe9",
       "-no-reboot", "-display", "none", "-serial", "stdio",
       "-qmp", "unix:%s,server=on,wait=off" % qmp_path,
       "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:22" % args.port, "-device", "e1000,netdev=n0",
       "-device", "qemu-xhci,id=xhci", "-device", "usb-tablet,bus=xhci.0"]
open(args.log, "w").close()
q = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
deadline = time.time() + args.timeout

def finish(rc=None):
    try:
        q.kill(); q.wait(5)
    except Exception:
        pass
    shutil.rmtree(tmpd, ignore_errors=True)
    note("DONE fails=%d" % fails[0])
    sys.exit(rc if rc is not None else (1 if fails[0] else 0))

def logtext():
    try:
        with open(args.log, "rb") as f: return f.read().decode("utf-8", "replace")
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

def serial(b, delay=0.15):
    q.stdin.write(b); q.stdin.flush(); time.sleep(delay)

def type_serial(s):
    serial(b"\x15" + s.encode() + b"\r", 0.3)

def hexbytes(b):
    return " ".join("%02x" % x for x in b)

# Expected file after every session's edit: L1 Enter L2 Enter L3 typed, line 2
# cut (^K) and pasted (^U) on the magic last line below L3.
def expected(l1, l2, l3):
    return (l1 + "\n" + l3 + "\n" + l2 + "\n").encode()

def od_check(name, path, want, after):
    """Ask the console bash for the file bytes (one hex line, ODHEX marker)."""
    pos = len(logtext())
    type_serial("  echo ODHEX-$(od -An -tx1 -v %s | tr -s ' \\n' '  ')-END" % path)
    m = wait_for(r"ODHEX-([0-9a-f ]*)-END", 20, pos)
    got = " ".join(m.group(1).split()) if m else "?"
    check(name, got == hexbytes(want), "got=[%s] want=[%s]" % (got, hexbytes(want)))

# ---- boot ---------------------------------------------------------------------
if not wait_for(r"bash-5\.2# ", 150):
    note("boot FAIL (no bash prompt)"); finish(1)
check("boot-nano-installed", re.search(r"\[NANO\] installed GNU nano to /usr/bin/nano", logtext()) is not None)
time.sleep(2)

# ---- QMP: keyboard, tablet, screendumps ------------------------------------------
qs = None
for _ in range(100):
    try:
        qs = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); qs.connect(qmp_path); break
    except OSError:
        qs = None; time.sleep(0.1)
if not check("qmp", qs is not None):
    finish(1)
qf = qs.makefile("rwb")
def qmp_read():
    while True:
        line = qf.readline()
        if not line: return None
        msg = json.loads(line)
        if "event" in msg: continue
        return msg
qmp_read()
def qmpc(name, **a):
    qf.write((json.dumps({"execute": name, "arguments": a}) + "\n").encode()); qf.flush()
    return qmp_read()
qmpc("qmp_capabilities")
def sendkey(k, hold=50):
    qmpc("human-monitor-command", **{"command-line": "sendkey %s %d" % (k, hold)}); time.sleep(0.12)
US = {" ": "spc", "-": "minus", ".": "dot", "/": "slash", "|": "shift-backslash", "$": "shift-4",
      "(": "shift-9", ")": "shift-0", "'": "apostrophe", "_": "shift-minus", "=": "equal"}
IT = {" ": "spc", ".": "dot", "/": "shift-7", "à": "apostrophe", "è": "bracket_left"}
def type_keys(s, layout=US, enter=True):
    for ch in s:
        if ch.isascii() and ch.isalpha() and ch.isupper(): sendkey("shift-" + ch.lower())
        elif ch.isascii() and ch.isalnum(): sendkey(ch)
        else: sendkey(layout[ch])
    if enter: sendkey("ret")
W = H = 0
def move(x, y):
    ax, ay = int(x * 32767 / max(1, W - 1)), int(y * 32767 / max(1, H - 1))
    qmpc("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": ax}},
                                     {"type": "abs", "data": {"axis": "y", "value": ay}}])
    time.sleep(0.15)
def button(down):
    qmpc("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
    time.sleep(0.15)
shot = [0]
def screendump(name):
    shot[0] += 1
    path = os.path.join(args.shots or tmpd, "nano_%02d_%s.ppm" % (shot[0], name))
    if os.path.exists(path): os.unlink(path)
    qmpc("screendump", filename=path)
    for _ in range(50):
        if os.path.exists(path) and os.path.getsize(path) > 100: break
        time.sleep(0.1)
    time.sleep(0.3)
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]
def pixel(img, x, y):
    w, h, d = img
    o = (y * w + x) * 3
    return (d[o], d[o + 1], d[o + 2])
def light(px): return sum(px) > 450


# ============================================================== ssh session ====
if "ssh" in modes:
    key = os.path.join(tmpd, "id")
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "secos-nano", "-f", key], check=True)
    pub = open(key + ".pub").read().strip()
    type_serial("  mkdir -p /root/.ssh; echo '%s' > /root/.ssh/authorized_keys; echo KEY-$((6*7))-SET" % pub)
    check("ssh-key-installed", wait_for(r"KEY-42-SET", 20) is not None)
    sshopt = ["-F", "/dev/null", "-p", str(args.port), "-i", key, "-o", "IdentitiesOnly=yes", "-o", "StrictHostKeyChecking=no",
              "-o", "UserKnownHostsFile=/dev/null", "-o", "BatchMode=yes", "-o", "LogLevel=ERROR",
              "-o", "ConnectTimeout=10"]
    up = False
    for _ in range(30):                        # init brings up DHCP + dropbear
        r = subprocess.run(["ssh"] + sshopt + ["root@127.0.0.1", "echo UP"], stdin=subprocess.DEVNULL,
                           capture_output=True, timeout=30)
        if b"UP" in r.stdout: up = True; break
        time.sleep(3)
    check("ssh-reachable", up)
    if up:
        subprocess.run(["ssh"] + sshopt + ["root@127.0.0.1", "rm -f /tmp/nanossh.txt"],
                       stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
        pid, master = pty.fork()
        if pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 90, 0, 0))
            os.environ["TERM"] = "xterm-256color"
            os.execvp("ssh", ["ssh", "-tt"] + sshopt + ["root@127.0.0.1", "nano /tmp/nanossh.txt"])
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 90, 0, 0))
        buf = bytearray()
        def pump(t):
            end = time.time() + t
            while time.time() < end:
                r, _, _ = select.select([master], [], [], 0.1)
                if r:
                    try: d = os.read(master, 65536)
                    except OSError: return False
                    if not d: return False
                    buf.extend(d)
            return True
        def until(pat, t=20, after=0):
            rx = re.compile(pat)
            end = time.time() + t
            while time.time() < end:
                if rx.search(bytes(buf), after): return True
                if not pump(0.3): return rx.search(bytes(buf), after) is not None
            return False
        def send(b, t=0.4):
            os.write(master, b); pump(t)
        ok = until(rb"GNU nano 8\.7", 30)
        check("ssh-nano-starts", ok)
        check("ssh-nano-30-rows", re.search(rb"\x1b\[30;1H|\x1b\[30d|\x1b\[29d|\x1b\[29;", bytes(buf)) is not None)
        L1, L2, L3 = "prima riga àè", "seconda riga", "terza riga"
        send((L1 + "\r" + L2 + "\r" + L3).encode(), 1.0)   # no Enter after L3: no empty line
        check("ssh-utf8-echoed", "àè".encode() in bytes(buf))
        p = len(buf)
        send(b"\x17", 0.8)                       # ^W Where Is
        send(b"seconda\r", 1.0)
        check("ssh-search", until(rb"Search Wrapped", 10, p), "(^W from line 3 wraps to line 2)")
        send(b"\x0b", 0.8)                       # ^K cut line 2
        send(b"\x1bOB", 0.6)                     # Down: the empty last line (keypad mode)
        send(b"\x15", 0.8)                       # ^U paste
        p = len(buf)
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 100, 0, 0))
        os.kill(pid, signal.SIGWINCH)
        pump(3)
        post = bytes(buf[p:])
        check("ssh-winch-redraw", b"GNU nano" in post and
              re.search(rb"\x1b\[(39|40)(;\d+)?[Hd]", post) is not None,
              "(repaint after 30x90 -> 40x100: %d bytes)" % len(post))
        p = len(buf)
        send(b"\x0f", 1.0)                       # ^O write out
        send(b"\r", 1.5)
        check("ssh-saved", until(rb"Wrote 3 lines", 10, p))
        send(b"\x18", 1.5)                       # ^X exit
        ended = False
        for _ in range(40):
            done, st = os.waitpid(pid, os.WNOHANG)
            if done: ended = True; break
            pump(0.25)
        check("ssh-nano-exits", ended)
        if not ended:
            try: os.kill(pid, signal.SIGKILL)
            except OSError: pass
        os.close(master)
        r = subprocess.run(["ssh"] + sshopt + ["root@127.0.0.1", "od -An -tx1 -v /tmp/nanossh.txt"],
                           stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
        got = " ".join(r.stdout.decode().split())
        want = hexbytes(expected(L1, L2, L3))
        check("ssh-file-bytes", got == want, "got=[%s] want=[%s]" % (got, want))

# ========================================================== console session ====
if "con" in modes:
    pos = len(logtext())
    type_serial("  rm -f /tmp/nanocon.txt; echo CONTERM-$TERM-END; nano /tmp/nanocon.txt")
    mterm = wait_for(r"CONTERM-([^$\s]*)-END", 20, pos)
    check("con-term-linux", mterm is not None and mterm.group(1) == "linux",
          "(console TERM=%s)" % (mterm.group(1) if mterm else "?"))
    check("con-nano-starts", wait_for(r"GNU nano 8\.7", 20, pos) is not None)
    time.sleep(1)
    L1, L2, L3 = "console àè", "seconda riga", "terza riga"
    serial((L1 + "\r" + L2 + "\r" + L3).encode(), 1.0)
    # The framebuffer console (8x16 cells): nano's reverse-video title bar on
    # row 0 across the screen, the typed text on rows 1..3.
    img = screendump("console")
    cols = img[0] // 8
    title = sum(1 for c in range(cols) if light(pixel(img, c * 8, 0)))
    ink = sum(1 for r in (1, 2, 3) for y in range(r * 16, r * 16 + 16) for x in range(0, 160)
              if light(pixel(img, x, y)))
    check("con-fb-screen", title > cols * 9 // 10 and ink > 100, "(title cells lit=%d/%d, text pixels=%d)" % (title, cols, ink))
    serial(b"\x17", 0.8); serial(b"seconda\r", 1.0)
    check("con-search", wait_for(r"Search Wrapped", 10, pos) is not None)
    serial(b"\x0b", 0.8)
    serial(b"\x1b[B", 0.6)                       # Down (linux console: CSI B)
    serial(b"\x15", 0.8)
    p = len(logtext())
    serial(b"\x0f", 1.0); serial(b"\r", 1.5)
    check("con-saved", wait_for(r"Wrote 3 lines", 10, p) is not None)
    serial(b"\x18", 1.5)
    check("con-nano-exits", wait_for(r"bash-5\.2# ", 15, p) is not None)
    od_check("con-file-bytes", "/tmp/nanocon.txt", expected(L1, L2, L3), pos)
    # Edit the existing file: read it back, insert at the top, overwrite it.
    p = len(logtext())
    type_serial("  nano /tmp/nanocon.txt")
    check("con-reopen-reads", wait_for(r"Read 3 lines", 15, p) is not None)
    time.sleep(1)
    serial("nuova \u00f2\r".encode(), 1.0)
    serial(b"\x0f", 1.0); serial(b"\r", 1.5)
    check("con-overwrite-saved", wait_for(r"Wrote 4 lines", 10, p) is not None)
    serial(b"\x18", 1.5)
    wait_for(r"bash-5\.2# ", 15, p)
    od_check("con-overwrite-bytes", "/tmp/nanocon.txt", "nuova \u00f2\n".encode() + expected(L1, L2, L3), p)

# ============================================================== gui session ====
if "gui" in modes:
    pos = len(logtext())
    type_serial("  /bin/startgui --trace")
    m = wait_for(r"SW-READY (\d+)x(\d+)", 40, pos)
    if not check("gui-ready", m is not None):
        finish(1)
    W, H = int(m.group(1)), int(m.group(2))
    mt = wait_for(r"SW-TERM-OPEN 1 pid=\d+ pts/\d+ at (-?\d+),(-?\d+)", 30, pos)
    if not check("gui-term-open", mt is not None):
        finish(1)
    tx, ty = int(mt.group(1)), int(mt.group(2))
    def cell(c, r):                            # top-left pixel of terminal cell (c, r)
        return tx + 1 + 4 + c * 8, ty + 26 + 4 + r * 16
    time.sleep(2.5)
    type_keys("rm -f /tmp/nanogui.txt")
    p = len(logtext())
    type_keys("loadkeys it")
    check("gui-layout-it", wait_for(r"SW-LAYOUT it", 15, p) is not None)
    time.sleep(0.5)
    type_keys("nano /tmp/nanogui.txt", IT)
    time.sleep(3)
    img = screendump("nano_open")
    title = sum(1 for c in range(0, 80) if light(pixel(img, cell(c, 0)[0] + 1, cell(c, 0)[1] + 1)))
    help23 = light(pixel(img, cell(0, 22)[0] + 1, cell(0, 22)[1] + 1))
    check("gui-nano-screen", title > 70 and help23, "(title cells lit=%d/80, help bar on row 23: %s)" % (title, help23))
    L1, L2, L3 = "prima riga àè", "seconda riga", "terza riga"
    type_keys(L1, IT); type_keys(L2, IT); type_keys(L3, IT, enter=False)
    sendkey("ctrl-w"); time.sleep(0.5)
    type_keys("seconda", IT); time.sleep(0.8)
    sendkey("ctrl-k"); time.sleep(0.5)
    sendkey("down"); time.sleep(0.5)
    sendkey("ctrl-u"); time.sleep(0.8)
    # Shrink the window with the grip: 80x24 -> 60x18 (as the GUI gate does).
    gx, gy = tx + 650 - 5, ty + 419 - 5
    p = len(logtext())
    move(gx, gy); button(True)
    for i in range(1, 5): move(gx - i * 40, gy - i * 24)
    button(False)
    check("gui-resize", wait_for(r"SW-RESIZE 1 60x18", 10, p) is not None)
    time.sleep(2.5)
    img = screendump("nano_resized")
    help17 = light(pixel(img, cell(0, 16)[0] + 1, cell(0, 16)[1] + 1))
    title60 = sum(1 for c in range(0, 60) if light(pixel(img, cell(c, 0)[0] + 1, cell(c, 0)[1] + 1)))
    check("gui-winch-redraw", help17 and title60 > 55,
          "(help bar now on row 17 of 18: %s, title cells lit=%d/60)" % (help17, title60))
    sendkey("ctrl-o"); time.sleep(1)
    sendkey("ret"); time.sleep(1.5)
    sendkey("ctrl-x"); time.sleep(1.5)
    p = len(logtext())
    type_keys("loadkeys us", IT)
    check("gui-layout-us", wait_for(r"SW-LAYOUT us", 15, p) is not None)
    p = len(logtext())
    type_keys("echo GUIOD $(od -An -tx1 -v /tmp/nanogui.txt) END")
    mo = wait_for(r"SW-TTY 1: GUIOD ([0-9a-f ]*) END", 15, p)
    got = " ".join(mo.group(1).split()) if mo else "?"
    want = hexbytes(expected(L1, L2, L3))
    check("gui-file-bytes", got == want, "got=[%s] want=[%s]" % (got, want))
    p = len(logtext())
    sendkey("ctrl-alt-backspace")
    check("gui-exit", wait_for(r"SW-EXIT", 15, p) is not None)
    time.sleep(2)

lt = logtext()
check("no-ring3-fault-in-nano", re.search(r"\[EXC\].*ring3", lt) is None)
check("no-panic", re.search(r"\[PANIC\]|\[EXC\].*ring0", lt) is None)
check("no-enosys", "secos-musl: ENOSYS" not in lt)
finish()
