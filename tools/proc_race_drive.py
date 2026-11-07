#!/usr/bin/env python3
# tools/proc_race_drive.py — process-lifetime race stress (M50 B1).
# SPDX-License-Identifier: MIT
#
# Boots a SecOS ISO built with CONSOLE=root (ideally also -DSECOS_LOCKDEP:
# lock checks + kfree poisoning with 0x6B) in QEMU with -smp N, waits for the
# console bash prompt and runs /bin/smptest procrace: readers doing kill -0 and
# reading /proc/<pid>/{status,maps,stat} of random pids while churners fork,
# mmap/mprotect/munmap, grow/shrink the heap, orphan grandchildren and get
# SIGKILLed (see user/smptest.c). Plus a shell-level variant of the same race
# (bash `kill -0` + `cat /proc/<pid>/...` loops against fork bursts).
#
# One warm-up run, then measured runs with MemFree (/proc/meminfo) sampled
# after each: the free memory must be steady (every reaped process gave its
# frames back; a reference leak would pin zombies and their address spaces).
# The debugcon log must stay free of kernel fault markers.
#
# Output: one "[PRD] <step> OK|FAIL ..." line per step and "[PRD] RESULT ok=N
# fail=M". Exit 0 when every step passed.
#
# Usage: proc_race_drive.py --iso ISO --smp N [--mem 1G] [--log LOG]
#                           [--secs S] [--runs R] [--qemu BIN] [--boot SECS]
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
    ap.add_argument("--log", default="/tmp/secos_proc_race.log")
    ap.add_argument("--secs", type=int, default=30)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--boot", type=float, default=240)
    o = ap.parse_args()

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
    def fault():
        try: d = open(o.log, "rb").read().decode("latin1")
        except OSError: d = ""
        for m in FAIL_MARKERS:
            if m in d: return m
        return None
    ok = fail = 0
    def report(name, good, why=""):
        nonlocal ok, fail
        if good: ok += 1; print(f"[PRD] {name} OK {why}".rstrip(), flush=True)
        else: fail += 1; print(f"[PRD] {name} FAIL {why}".rstrip(), flush=True)
    idx = [0]
    def run(cmd, tmo):
        sentinel = f"PRD-END-{idx[0]}"
        line = f"{cmd}; echo PRD-END-$(({idx[0]}+0))\r"
        idx[0] += 1
        with lock: since = len(buf)
        p.stdin.write(line.encode()); p.stdin.flush()
        done = wait_for(sentinel, tmo, since)
        with lock: out = bytes(buf[since:]).decode("latin1")
        return done, out
    # [M52] MemFree alone counts the kernel heap's growth as a leak, but the
    # heap keeps every frame it ever took (a high-water mark that moves when
    # more syscalls run at once). Sample MemFree + Slab (frames used outside
    # the heap: a frame leak) and SUnreclaim (bytes in heap blocks: a kmalloc
    # leak) and check each; a kernel without the fields reports Slab 0.
    def memfree():
        done, out = run("echo MF-$(grep -E '^(MemFree|Slab|SUnreclaim):' /proc/meminfo | tr -s ' ' | cut -d' ' -f2 | tr '\\n' '-')KB", 60)
        m = re.findall(r"MF-(\d+)-(\d+)-(\d+)-KB", out)
        if done and m: return (int(m[-1][0]) + int(m[-1][1]), int(m[-1][2]), int(m[-1][0]))
        m = re.findall(r"MF-(\d+)-KB", out)
        return (int(m[-1]), 0, int(m[-1])) if done and m else None
    def finish():
        report("no-kernel-fault", fault() is None, str(fault() or ""))
        p.kill()
        print(f"[PRD] RESULT ok={ok} fail={fail}", flush=True)
        sys.exit(0 if fail == 0 else 1)

    t0 = time.time(); prompt = False
    while time.time() - t0 < o.boot and alive[0]:
        with lock: tail = bytes(buf[-64:])
        if tail.rstrip().endswith(b"#"): prompt = True; break
        time.sleep(0.2)
    report("boot-prompt", prompt)
    if not prompt: finish()
    time.sleep(1.0)

    tmo = o.secs + 240
    # Warm-up: kernel heap and page-cache growth happen here, not in the
    # measured runs.
    done, out = run(f"/bin/smptest procrace 5 8 4", 300)
    report("warmup", done and "[smptest] procrace PASS" in out,
           (re.findall(r"\[smptest\] procrace [^\r\n]*", out) or ["timeout"])[-1])
    for l in sorted(set(re.findall(r"\[procrace\][^\r\n]*", out)))[:8]:
        print(f"[PRD] note {l}", flush=True)
    # Shell-level variant: bash's own kill -0 / cat /proc/<pid>/... against
    # bursts of short-lived children (pids derived from $$ and $!).
    shell = ("( for i in $(seq 1 40); do ( for j in 1 2 3 4; do /bin/true & done; wait ) & "
             "q=$!; kill -0 $q 2>/dev/null; cat /proc/$q/status /proc/$q/maps >/dev/null 2>&1; "
             "for d in 1 2 3 5 8; do cat /proc/$((q+d))/maps /proc/$((q+d))/status >/dev/null 2>&1; "
             "kill -0 $((q+d)) 2>/dev/null; done; wait; done ) & "
             "( for i in $(seq 1 150); do for pp in /proc/[0-9]*; do cat $pp/status >/dev/null 2>&1; done; done ) & "
             "wait; echo SHRACE-$((6*7))")
    done, out = run(shell, 600)
    report("shell-race", done and "SHRACE-42" in out, "" if done else "timeout")
    mf = []
    for r in range(o.runs):
        done, out = run(f"/bin/smptest procrace {o.secs} 8 4", tmo)
        report(f"procrace-{r+1}", done and "[smptest] procrace PASS" in out,
               (re.findall(r"\[smptest\] procrace [^\r\n]*", out) or ["timeout"])[-1])
        for l in sorted(set(re.findall(r"\[procrace\][^\r\n]*", out)))[:8]:
            print(f"[PRD] note {l}", flush=True)
        time.sleep(2.0)                 # let the reaper finish (idle loops)
        v = memfree()
        mf.append(v)
        print(f"[PRD] memfree after run {r+1}: {v} kB", flush=True)
        if fault(): break
    def steady(vals):
        if len(vals) < 2 or None in vals: return False
        if len(vals) > 2: vals = vals[1:]
        return max(vals) - min(vals) <= MEMFREE_SLACK_KB
    good = None not in mf and steady([v[0] for v in mf])
    report("memfree-steady", bool(good), f"MemFree+Slab_kB={[v and v[0] for v in mf]} "
           f"MemFree_kB={[v and v[2] for v in mf]} slack={MEMFREE_SLACK_KB}")
    good = None not in mf and steady([v[1] for v in mf])
    report("heap-in-use-steady", bool(good), f"SUnreclaim_kB={[v and v[1] for v in mf]} slack={MEMFREE_SLACK_KB}")
    finish()

if __name__ == "__main__":
    main()
