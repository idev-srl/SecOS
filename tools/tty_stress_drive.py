#!/usr/bin/env python3
# tools/tty_stress_drive.py — M51 C5 TTY / pty gate driver (selftest "M51 TTY").
# SPDX-License-Identifier: MIT
#
# Boots a SecOS ISO built with CONSOLE=root (and, for --tttest, with
# -DSECOS_TTY_TEST -DSECOS_LOCKDEP) in QEMU with -smp N, a serial console on
# stdio and a private QMP unix socket, then:
#   1. --tttest: the in-kernel forced-concurrency test (kernel/tty_test.c)
#      must print "[TTYTEST] RESULT PASS" on debugcon, and its negative
#      control must have seen damage without the lock;
#   2. /bin/smptest pty (16 pty pairs, raw both ways + canonical with echo)
#      and /bin/smptest pipe (pipelines with every byte checked);
#   3. typing while a background job writes to the console: a line typed on
#      the PS/2 keyboard (QMP sendkey -> IRQ1 -> line discipline) and one
#      typed on the serial line must reach `read` intact;
#   4. job control from the keyboard IRQ (lock-free ISIG snapshot): Ctrl-C
#      ends a foreground `sleep 30` at once, Ctrl-Z stops a `sleep 40`;
#   5. the debugcon log stays free of [LOCKDEP], "[LOCK] BUG", [EXC], panics.
# Output: "[TSD] <step> OK|FAIL ..." and "[TSD] RESULT ok=N fail=M"; exit 0
# when every step passed. Never binds a host TCP port; only kills its QEMU.
#
# Usage: tty_stress_drive.py --iso ISO [--smp 4] [--log LOG] [--tttest]
import argparse, json, os, re, socket, subprocess, sys, tempfile, threading, time

ap = argparse.ArgumentParser()
ap.add_argument("--iso", required=True)
ap.add_argument("--smp", type=int, default=4)
ap.add_argument("--mem", default="1G")
ap.add_argument("--log", default="/tmp/secos_tty_stress.log")
ap.add_argument("--boot", type=float, default=300)
ap.add_argument("--tttest", action="store_true")
o = ap.parse_args()

FAIL_MARKERS = ["[EXC] int=", "[PANIC]", "nested fault", "stack smashing", "LOCKDEP", "[LOCK] BUG"]
tmpd = tempfile.mkdtemp(prefix="secos_tsd_")
qmp_path = os.path.join(tmpd, "qmp.sock")
cmd = ["qemu-system-x86_64", "-cdrom", o.iso, "-boot", "d", "-m", o.mem, "-smp", str(o.smp),
       "-debugcon", "file:" + o.log, "-global", "isa-debugcon.iobase=0xe9",
       "-no-reboot", "-display", "none", "-serial", "stdio",
       "-qmp", "unix:%s,server=on,wait=off" % qmp_path]
open(o.log, "w").close()
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
buf = bytearray(); lock = threading.Lock(); alive = [True]
def rd():
    while True:
        b = p.stdout.read1(65536)
        if not b: alive[0] = False; break
        with lock: buf.extend(b)
threading.Thread(target=rd, daemon=True).start()

ok = fail = 0
def report(name, good, why=""):
    global ok, fail
    if good: ok += 1
    else: fail += 1
    print(f"[TSD] {name} {'OK' if good else 'FAIL'} {why}".rstrip(), flush=True)
def finish():
    try: p.kill()
    except OSError: pass
    print(f"[TSD] RESULT ok={ok} fail={fail}", flush=True)
    sys.exit(0 if fail == 0 else 1)
def logtext():
    try:
        with open(o.log, "rb") as f: return f.read().decode("latin-1")
    except OSError: return ""
def fault():
    d = logtext()
    for m in FAIL_MARKERS:
        if m in d: return m
    return None
def wait_ser(needle, timeout, since=0):
    nb = needle.encode(); t0 = time.time()
    while time.time() - t0 < timeout:
        with lock:
            if nb in buf[since:]: return True
        if not alive[0]: return False
        time.sleep(0.05)
    return False
def ser_since():
    with lock: return len(buf)
def ser_text(since):
    with lock: return bytes(buf[since:]).decode("latin1")
def type_serial(s):
    p.stdin.write(s.encode()); p.stdin.flush()

# ---- QMP ------------------------------------------------------------------------
qs = None
for _ in range(100):
    try:
        qs = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); qs.connect(qmp_path); break
    except OSError:
        qs = None; time.sleep(0.1)
if not qs:
    report("qmp-connect", False); finish()
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
def sendkey(k, hold=40):
    qmp("human-monitor-command", **{"command-line": "sendkey %s %d" % (k, hold)}); time.sleep(0.08)
def type_keys(word):
    for ch in word: sendkey(ch)

# ---- 1. in-kernel test ----------------------------------------------------------
if o.tttest:
    t0 = time.time(); res = None
    while time.time() - t0 < o.boot and alive[0]:
        m = re.search(r"\[TTYTEST\] RESULT (PASS|FAIL)", logtext())
        if m: res = m.group(1); break
        time.sleep(0.5)
    d = logtext()
    for line in re.findall(r"\[TTYTEST\][^\n]*", d): print("   " + line)
    report("ttytest-kernel", res == "PASS", f"result={res}")
    report("ttytest-negative-control", "damage seen without the lock" in d)
    report("ttytest-esc-wait-ran", bool(re.search(r"C ESC waits \(lock dropped, re-checked\): [1-9]", d)))

# ---- bash prompt ----------------------------------------------------------------
t0 = time.time(); prompt = False
while time.time() - t0 < o.boot and alive[0]:
    # [M52] init messages can land after bash's first prompt (they run in
    # parallel): once bash printed one, ask for a fresh prompt now and then.
    with lock: tail = bytes(buf[-64:]); seen = b"bash-" in buf
    if tail.rstrip().endswith(b"#"): prompt = True; break
    if seen and time.time() - globals().get("_m52_poke", 0.0) > 5.0:
        globals()["_m52_poke"] = time.time(); p.stdin.write(b"\r"); p.stdin.flush()
    time.sleep(0.2)
report("boot-prompt", prompt)
if not prompt: finish()
time.sleep(1.0)

def run(name, line, expect, tmo):
    s = ser_since()
    type_serial("\x15" + line + "\r")
    good = wait_ser(expect, tmo, s)
    m = fault()
    report(name, good and not m, "" if good else f"timeout {tmo}s waiting for {expect!r}" + (f" marker {m!r}" if m else ""))
    return s

# ---- 2. userland pty / pipe stress ------------------------------------------------
run("smptest-pty", "/bin/smptest pty 16 64", "[smptest] pty PASS", 900)
run("smptest-pipe", "/bin/smptest pipe 3 3 512", "[smptest] pipe PASS", 900)

# ---- 3. typing while a background job writes ---------------------------------------
BG = "( i=0; while [ $i -lt 400 ]; do echo bgjob-line-$i; i=$((i+1)); done ) & read -r L; wait; echo \"GOT-[$L]-$((6*7))\""
s = ser_since()
type_serial("\x15" + BG + "\r")
time.sleep(1.5)
type_keys("typedunderload"); sendkey("ret")
good = wait_ser("GOT-[typedunderload]-42", 180, s)
report("ps2-typing-under-output", good, "" if good else "line lost or damaged")

s = ser_since()
type_serial("\x15" + BG + "\r")
time.sleep(1.5)
type_serial("serialtyped\r")
good = wait_ser("GOT-[serialtyped]-42", 180, s)
report("serial-typing-under-output", good, "" if good else "line lost or damaged")

# ---- 4. job control from the keyboard IRQ ---------------------------------------
s = ser_since()
type_serial("\x15sleep 30\r")
time.sleep(2.5)
t_int = time.time()
sendkey("ctrl-c")
time.sleep(0.5)
type_serial("\x15echo INTR-$((6*7))\r")
good = wait_ser("INTR-42", 20, s)
report("ctrl-c-ends-foreground", good and time.time() - t_int < 20,
       "" if good else "sleep not interrupted")

s = ser_since()
type_serial("\x15sleep 40\r")
time.sleep(2.5)
sendkey("ctrl-z")
good = wait_ser("Stopped", 20, s)
report("ctrl-z-stops-foreground", good)
s = ser_since()
type_serial("\x15kill -9 %1; wait; echo JC-$((6*7))\r")
report("job-killed", wait_ser("JC-42", 30, s))

# ---- 5. kernel markers ----------------------------------------------------------
m = fault()
report("no-kernel-fault", m is None, str(m or ""))
finish()
