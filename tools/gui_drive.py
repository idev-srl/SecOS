#!/usr/bin/env python3
# tools/gui_drive.py — M40 GUI kernel-interface gate driver (selftest).
# SPDX-License-Identifier: MIT
#
# Boots the SecOS ISO in QEMU into the bash login shell, types commands over
# the serial console and injects input through QMP (HMP sendkey / mouse_move /
# mouse_button / mouse_set, QMP input-send-event for the absolute usb-tablet).
# Screenshots (QMP screendump, PPM) check that /dev/fb0 really shows what a
# client drew and that the text console comes back after the client exits or
# crashes. Results:
#   - the guest's debugcon log (--log) carries the in-guest G?- markers;
#   - this driver appends "[GUI-DRV] <check> OK|FAIL ..." lines to --out.
# Scenarios: --scenario main (PS/2 keyboard + PS/2 mouse + usb-tablet +
# usb-mouse) or --scenario usbkbd (adds usb-kbd: the USB HID keyboard path).
#
# Never binds host TCP ports (QMP is a private unix socket); only kills the
# QEMU it started.
import argparse, json, os, re, socket, subprocess, sys, tempfile, threading, time

ap = argparse.ArgumentParser()
ap.add_argument("--iso", default="myos.iso")
ap.add_argument("--log", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--scenario", default="main", choices=["main", "usbkbd"])
ap.add_argument("--timeout", type=float, default=240)
args = ap.parse_args()

tmpd = tempfile.mkdtemp(prefix="secos_gui_")
qmp_path = os.path.join(tmpd, "qmp.sock")
devs = ["-device", "qemu-xhci,id=xhci", "-device", "usb-tablet,bus=xhci.0", "-device", "usb-mouse,bus=xhci.0"]
if args.scenario == "usbkbd":
    devs += ["-device", "usb-kbd,bus=xhci.0"]
cmd = ["qemu-system-x86_64", "-cdrom", args.iso, "-boot", "d", "-m", "512M",
       "-debugcon", "file:" + args.log, "-global", "isa-debugcon.iobase=0xe9",
       "-no-reboot", "-display", "none", "-serial", "stdio",
       "-qmp", "unix:%s,server=on,wait=off" % qmp_path] + devs
open(args.log, "w").close()
out = open(args.out, "a")
def note(s):
    out.write("[GUI-DRV] " + s + "\n"); out.flush()
    print("[GUI-DRV] " + s, flush=True)

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
        txt = logtext()
        m = rx.search(txt, after)
        if m: return m
        if q.poll() is not None: return None
        time.sleep(0.2)
    return None

def type_line(s):
    # Ctrl-U first: drop anything left on the bash line.
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
qmp_read()  # greeting
def qmp(cmdname, **a):
    qf.write((json.dumps({"execute": cmdname, "arguments": a}) + "\n").encode()); qf.flush()
    return qmp_read()
qmp("qmp_capabilities")
def hmp(line):
    r = qmp("human-monitor-command", **{"command-line": line})
    return (r or {}).get("return", "")
def sendkey(k, hold=60):
    hmp("sendkey %s %d" % (k, hold)); time.sleep(0.25)
def abs_move(x, y):
    qmp("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": x}},
                                    {"type": "abs", "data": {"axis": "y", "value": y}}])

def screendump(name):
    path = os.path.join(tmpd, name + ".ppm")
    qmp("screendump", filename=path)
    for _ in range(50):
        if os.path.exists(path) and os.path.getsize(path) > 100: break
        time.sleep(0.1)
    time.sleep(0.3)
    with open(path, "rb") as f: data = f.read()
    # P6\n<w> <h>\n255\n<rgb...>
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]
def pixel(img, x, y):
    w, h, d = img
    o = (y * w + x) * 3
    return (d[o], d[o + 1], d[o + 2])

def check(name, cond, extra=""):
    note("%s %s %s" % (name, "OK" if cond else "FAIL", extra))

# ---- boot ------------------------------------------------------------------------
if not wait_for(r"bash-5\.2# ", 90):
    note("boot FAIL (no bash prompt)"); q.kill(); sys.exit(1)
time.sleep(2)

mice = hmp("info mice")
idx = {}
for line in mice.splitlines():
    m = re.search(r"Mouse #(\d+): (.*)", line)
    if not m: continue
    n, nm = int(m.group(1)), m.group(2)
    if "PS/2" in nm: idx["ps2"] = n
    elif "Tablet" in nm: idx["tablet"] = n
    elif "Mouse" in nm: idx["usbmouse"] = n
note("mice " + json.dumps(idx))

if args.scenario == "main":
    # 1. fb0: ioctls, mmap round trip, refusals, fork sharing, no leak.
    pos = len(logtext())
    type_line("  /bin/guitest fb")
    check("guitest-fb-ran", wait_for(r"G-END fb", 60, pos) is not None)
    # 2. capability refusals.
    pos = len(logtext())
    type_line("  /bin/guitest_nocap nocap")
    check("guitest-nocap-ran", wait_for(r"G-END nocap", 30, pos) is not None)

    # 3. input events: PS/2 mouse, USB mouse, USB tablet, PS/2 keyboard.
    pos = len(logtext())
    type_line("  /bin/guitest input")
    if wait_for(r"GI-READY", 30, pos):
        time.sleep(1)
        if "ps2" in idx:
            hmp("mouse_set %d" % idx["ps2"]); time.sleep(0.3)
            hmp("mouse_move 10 5"); time.sleep(0.4)
            hmp("mouse_move -3 -7"); time.sleep(0.4)
            hmp("mouse_button 1"); time.sleep(0.4)
            hmp("mouse_button 0"); time.sleep(0.4)
            hmp("mouse_move 0 0 1"); time.sleep(0.4)
        if "usbmouse" in idx:
            hmp("mouse_set %d" % idx["usbmouse"]); time.sleep(0.3)
            hmp("mouse_move 12 -6"); time.sleep(0.5)
            hmp("mouse_button 2"); time.sleep(0.5)
            hmp("mouse_button 0"); time.sleep(0.5)
            hmp("mouse_move 0 0 -1"); time.sleep(0.5)
        if "tablet" in idx:
            hmp("mouse_set %d" % idx["tablet"]); time.sleep(0.3)
            abs_move(16384, 8192); time.sleep(0.5)
            qmp("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}]); time.sleep(0.5)
            qmp("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}]); time.sleep(0.5)
        sendkey("a"); time.sleep(0.5)
        sendkey("q")
        if wait_for(r"GI-FLOOD-READY", 20, pos):
            if "ps2" in idx: hmp("mouse_set %d" % idx["ps2"])
            for i in range(200):
                hmp("mouse_move 1 1")
            check("flood-sent", True, "200 PS/2 moves")
        check("guitest-input-ran", wait_for(r"G-END input", 40, pos) is not None)
    else:
        check("guitest-input-ran", False, "no GI-READY")

    # 4. keymaps over sendkey: IT layout (PS/2 keyboard).
    pos = len(logtext())
    type_line("  /bin/loadkeys it")
    check("loadkeys-it", wait_for(r"loadkeys: keyboard layout it", 20, pos) is not None)

if args.scenario in ("main", "usbkbd"):
    if args.scenario == "usbkbd":
        if "[KBD] keymap it" in logtext():
            check("boot-keymap-it", True, "(built with KEYMAP=it)")
        else:
            pos = len(logtext())
            type_line("  /bin/loadkeys it")
            check("loadkeys-it", wait_for(r"loadkeys: keyboard layout it", 20, pos) is not None)
    pos = len(logtext())
    type_line("  /bin/guitest keys")
    if wait_for(r"GK-READY layout=it", 20, pos):
        time.sleep(0.5)
        for k in ["less", "shift-less", "shift-7", "shift-dot", "shift-slash",
                  "alt_r-semicolon", "alt_r-apostrophe", "alt_r-bracket_left", "alt_r-bracket_right",
                  "shift-alt_r-bracket_left", "shift-alt_r-bracket_right",
                  "apostrophe", "bracket_left", "shift-bracket_left", "equal", "semicolon", "backslash",
                  "alt_r-minus", "alt_r-equal", "ctrl-alt_r-semicolon", "shift-2", "shift-3", "alt_r-e"]:
            sendkey(k)
        sendkey("q")
        check("guitest-keys-ran", wait_for(r"GK-BYTES", 20, pos) is not None)
    else:
        check("guitest-keys-ran", False, "no GK-READY")
    pos = len(logtext())
    type_line("  /bin/loadkeys us; /bin/loadkeys")
    wait_for(r"loadkeys: keyboard layout us", 20, pos)
    if args.scenario == "usbkbd":
        # USB keyboard through evdev: guitest input lists it and sees KEY_A.
        pos = len(logtext())
        type_line("  /bin/guitest input")
        if wait_for(r"GI-READY", 30, pos):
            time.sleep(1)
            sendkey("a"); sendkey("q")
            if wait_for(r"GI-FLOOD-READY", 20, pos):
                if "ps2" in idx: hmp("mouse_set %d" % idx["ps2"])
                for i in range(200): hmp("mouse_move 1 1")
            check("guitest-input-usbkbd-ran", wait_for(r"G-END input", 40, pos) is not None)

if args.scenario == "main":
    # 5. console handoff: the client's pixels are on screen, console output is
    #    suppressed, and the text console returns after exit / crash.
    for variant in ("exit", "crash"):
        pos = len(logtext())
        type_line("  /bin/guitest gfx" + (" crash" if variant == "crash" else ""))
        m = wait_for(r"GG-DRAWN (\d+)x(\d+)", 30, pos)
        if not m:
            check("gfx-%s-drawn" % variant, False); continue
        W, H = int(m.group(1)), int(m.group(2))
        time.sleep(2)
        img = screendump("gfx_" + variant)
        tl = pixel(img, W // 4, H // 4); tr = pixel(img, 3 * W // 4, H // 4)
        bl = pixel(img, W // 4, 3 * H // 4); ctr = pixel(img, W // 2, H // 2)
        check("gfx-%s-pixels" % variant,
              img[0] == W and img[1] == H and tl == (255, 0, 0) and tr == (0, 255, 0) and
              bl == (0, 0, 255) and ctr == (255, 0, 255),
              "size=%dx%d tl=%s tr=%s bl=%s ctr=%s" % (img[0], img[1], tl, tr, bl, ctr))
        # The console printed 20 lines meanwhile: the bottom text rows must
        # still be pure client pixels (blue / yellow), i.e. not drawn over.
        clean = all(pixel(img, x, y) in ((0, 0, 255), (255, 255, 0))
                    for y in range(H - 48, H, 3) for x in range(0, W, 7))
        check("gfx-%s-console-suppressed" % variant, clean)
        if not wait_for(r"\[VT\] text console restored", 30, pos):
            check("gfx-%s-restored-log" % variant, False); continue
        check("gfx-%s-restored-log" % variant, True)
        time.sleep(2)
        img2 = screendump("text_" + variant)
        ctr2 = pixel(img2, W // 2, H // 2); tl2 = pixel(img2, W // 4, H // 4)
        # Text console background is black; no client colour may survive.
        check("gfx-%s-text-back" % variant,
              ctr2 != (255, 0, 255) and tl2 != (255, 0, 0) and pixel(img2, W // 4, 3 * H // 4) != (0, 0, 255),
              "ctr=%s tl=%s" % (ctr2, tl2))
        # Some text is drawn (non-black pixels exist in the text area).
        lit = sum(1 for y in range(0, H, 4) for x in range(0, W // 2, 4) if pixel(img2, x, y) != (0, 0, 0))
        check("gfx-%s-text-drawn" % variant, lit > 50, "lit=%d" % lit)

pos = len(logtext())
type_line("  echo GUI-$((6*7))-END")
check("bash-responsive", wait_for(r"GUI-42-END", 30, pos) is not None)
q.kill()
try: q.wait(5)
except Exception: pass
