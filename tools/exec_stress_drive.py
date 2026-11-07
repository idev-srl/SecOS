#!/usr/bin/env python3
# tools/exec_stress_drive.py — userland exec stress (M51 C6).
# SPDX-License-Identifier: MIT
#
# Boots a SecOS ISO built with CONSOLE=root (ideally also -DSECOS_LOCKDEP) in
# QEMU with -smp N, waits for the console bash prompt and runs `bash -c true`
# 500 times in 4 parallel loops (every one is a fork + in-place execve of the
# signed 1.1 MB bash: read, Ed25519-verified and pinned per exec, no shared
# staging buffer), while a fifth loop executes a tampered copy of a signed
# program (one flipped byte), which must be refused every time. (The
# SYS_SPAWN path, which shares the loader, is covered by the M16/M46 gates and
# by the in-kernel concurrent-load test, kernel/exec_test.c.)
#
# One warm-up run, then two measured runs with MemFree (/proc/meminfo) sampled
# after each: the free memory must be steady (every exec gave its staging
# buffer and its old image back). The debugcon log must stay free of kernel
# fault markers, and processes must have run on more than one CPU.
#
# With the BKL on, every exec still serializes on it; the gate checks that the
# per-exec staging is correct and leak-free under real multi-CPU load, and
# records the rate (it is the number dropping the BKL around exec should
# improve).
#
# Output: one "[ESD] <step> OK|FAIL ..." line per step and "[ESD] RESULT ok=N
# fail=M". Exit 0 when every step passed.
#
# Usage: exec_stress_drive.py --iso ISO --smp N [--mem 1G] [--log LOG]
#                             [--count 500] [--qemu BIN] [--boot SECS]
import sys, time, subprocess, threading, re, argparse

FAIL_MARKERS = ["[EXC] int=", "[PANIC]", "PANIC", "nested fault", "stack smashing",
                "[HEAP] CORRUPTION", "CORRUPTION FOUND", "[TLB] BUG", "shootdown timeout",
                "[LOCKDEP]", "[LOCK] BUG", "[PROC] BUG"]
MEMFREE_SLACK_KB = 1024   # allowed drift between measured runs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", default="1G")
    ap.add_argument("--log", default="/tmp/secos_exec_stress.log")
    ap.add_argument("--count", type=int, default=500)
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--boot", type=float, default=240)
    o = ap.parse_args()
    per = max(1, o.count // 4)

    qemu = [o.qemu, "-cdrom", o.iso, "-boot", "d", "-m", o.mem, "-smp", str(o.smp),
            "-debugcon", "file:" + o.log, "-global", "isa-debugcon.iobase=0xe9",
            "-no-reboot", "-display", "none", "-serial", "stdio"]
    open(o.log, "w").close()
    p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    buf = bytearray(); lock = threading.Lock(); alive = [True]
    def rd():
        while True:
            b = p.stdout.read1(65536)
            if not b: alive[0] = False; break
            with lock: buf.extend(b)
    threading.Thread(target=rd, daemon=True).start()

    def wait_for(needle, timeout, since=0):
        nb = needle.encode(); t0 = time.time()
        while time.time() - t0 < timeout:
            with lock:
                if nb in buf[since:]: return True
            if not alive[0]: return False
            time.sleep(0.05)
        return False
    def dlog():
        try: return open(o.log, "rb").read().decode("latin1")
        except OSError: return ""
    def fault():
        d = dlog()
        for m in FAIL_MARKERS:
            if m in d: return m
        return None
    ok = fail = 0
    def report(name, good, why=""):
        nonlocal ok, fail
        if good: ok += 1; print(f"[ESD] {name} OK {why}".rstrip(), flush=True)
        else: fail += 1; print(f"[ESD] {name} FAIL {why}".rstrip(), flush=True)
    idx = [0]
    def run(cmd, tmo):
        sentinel = f"ESD-END-{idx[0]}"
        line = f"{cmd}; echo ESD-END-$(({idx[0]}+0))\r"
        idx[0] += 1
        with lock: since = len(buf)
        p.stdin.write(line.encode()); p.stdin.flush()
        done = wait_for(sentinel, tmo, since)
        with lock: out = bytes(buf[since:]).decode("latin1")
        return done, out
    def memfree():
        done, out = run("echo MF-$(grep MemFree /proc/meminfo | tr -s ' ' | cut -d' ' -f2)-KB", 60)
        m = re.findall(r"MF-(\d+)-KB", out)
        return int(m[-1]) if done and m else None
    def finish():
        report("no-kernel-fault", fault() is None, str(fault() or ""))
        p.kill()
        print(f"[ESD] RESULT ok={ok} fail={fail}", flush=True)
        sys.exit(0 if fail == 0 else 1)

    t0 = time.time(); prompt = False
    while time.time() - t0 < o.boot and alive[0]:
        with lock: tail = bytes(buf[-64:])
        if tail.rstrip().endswith(b"#"): prompt = True; break
        time.sleep(0.2)
    report("boot-prompt", prompt)
    if not prompt: finish()
    time.sleep(1.0)

    # A tampered copy of a signed program (the multi-call coreutils ELF: one
    # flipped byte in its code) and a check that it is refused at all.
    done, out = run("cp /bin/coreutils /tmp/esd_bad && "
                    "printf '\\001' | dd of=/tmp/esd_bad bs=1 seek=8192 conv=notrunc 2>/dev/null; "
                    "chmod 755 /tmp/esd_bad; /tmp/esd_bad true; echo BADRC-$?", 60)
    m = re.findall(r"BADRC-(\d+)", out)
    # 126 = the execve itself failed (bash: "cannot execute"); a tampered
    # image that ran would exit with the applet's own status instead.
    report("tampered-refused", done and bool(m) and m[-1] == "126", f"rc={m[-1] if m else '?'}")
    done, out = run("bash -c true; echo GOODRC-$?", 60)
    report("signed-bash-runs", done and "GOODRC-0" in out)

    def stress(n, tag):
        cmd = ("t0=$(cut -d' ' -f1 /proc/uptime); "
               + " ".join(f"( k=0; for i in $(seq 1 {n}); do bash -c true || k=$((k+1)); done; echo EF{j}-$k ) &"
                          for j in range(4))
               + f" ( b=0; for i in $(seq 1 {max(1, n // 5)}); do /tmp/esd_bad true 2>/dev/null; [ $? -eq 126 ] || b=$((b+1)); done; echo EB-$b ) &"
               + " wait; t1=$(cut -d' ' -f1 /proc/uptime); echo ETIME-$t0-$t1")
        done, out = run(cmd, 1800)
        fails = [int(x) for x in re.findall(r"EF\d-(\d+)", out)]
        bad = re.findall(r"EB-(\d+)", out)
        tm = re.findall(r"ETIME-([\d.]+)-([\d.]+)", out)
        secs = float(tm[-1][1]) - float(tm[-1][0]) if tm else 0.0
        good = done and len(fails) == 4 and sum(fails) == 0 and bad and bad[-1] == "0"
        rate = f"{4 * n / secs:.1f}/s" if secs > 0 else "?"
        report(tag, good, f"execs={4 * n} failed={sum(fails) if fails else '?'} "
                          f"tampered_accepted={bad[-1] if bad else '?'} secs={secs:.1f} rate={rate}")

    stress(max(5, per // 5), "warmup")
    mf = []
    for r in range(2):
        stress(per, f"stress-{r + 1}")
        time.sleep(2.0)                 # let the reaper finish (idle loops)
        v = memfree()
        mf.append(v)
        print(f"[ESD] memfree after run {r + 1}: {v} kB", flush=True)
        if fault(): break
    good = len(mf) == 2 and None not in mf and abs(mf[1] - mf[0]) <= MEMFREE_SLACK_KB
    report("memfree-steady", good, f"samples_kB={mf} slack={MEMFREE_SLACK_KB}")
    cpus = set(re.findall(r"\[SMP\] cpu=0x([0-9A-Fa-f]+) run pid", dlog()))
    report("ran-on-several-cpus", len(cpus) >= 2 or o.smp < 2, f"cpus={sorted(cpus)}")
    finish()

if __name__ == "__main__":
    main()
