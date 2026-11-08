#!/usr/bin/env python3
# tools/smp_stress_drive.py — SMP stress session over the serial console (M48).
# SPDX-License-Identifier: MIT
#
# Boots a SecOS ISO built with CONSOLE=root in QEMU with -smp N, waits for the
# console bash prompt and runs a fixed list of multicore workloads: CPU-bound
# background jobs, pipelines, fork/exec bursts, nested command substitution,
# the FPU-across-migration/signal check (/bin/smptest fpu), fork bursts with
# status checks (/bin/smptest burst), pty pairs and checked pipelines
# (/bin/smptest pty|pipe, M51) and several syscall fuzzers in parallel
# on all CPUs. Each step must print its expected text before a timeout, and
# the debugcon log must stay free of kernel fault markers. Finally /proc/smp
# must show the work actually spread (migrations, reschedule IPIs) and no
# TLB-shootdown timeouts.
#
# The first step checks that userland sees the CPU count the kernel brought
# online: nproc, nproc --all, /sys/devices/system/cpu/online, the
# Cpus_allowed_list of /proc/<pid>/status and /bin/smptest cpus (sysconf,
# get_nprocs, sched_getaffinity). --cpus-only runs just that step, against
# --expect-cpus (default --smp; 1 for an image booted with nosmp).
#
# Output: one "[SSD] <step> OK|FAIL ..." line per step and "[SSD] RESULT ok=N
# fail=M". Exit 0 when every step passed.
#
# Usage: smp_stress_drive.py --iso ISO --smp N [--mem 1G] [--log LOG]
#                            [--qemu BIN] [--cpu MODEL] [--boot SECS] [--quick]
#                            [--cpus-only] [--expect-cpus N]
import sys, time, subprocess, threading, re, argparse

STEPS = [
    # name, command, expected output, timeout (s). Expected markers come from
    # an arithmetic expansion ($((6*7)) -> 42) so the echoed command line,
    # which readline may wrap and redraw, can never match them by itself.
    ("spin-jobs",  "for r in 1 2 3; do for i in $(seq 1 12); do ( x=0; while [ $x -lt 4000 ]; do x=$((x+1)); done ) & done; wait; done; echo SPIN-$((6*7))",
                   "SPIN-42", 400),
    ("pipelines",  "for r in $(seq 1 10); do seq 1 3000 | sort -rn | uniq | wc -l > /tmp/ssd$r; done; echo PIPE-$(cat /tmp/ssd* | sort -u)",
                   "PIPE-3000", 400),
    ("exec-loop",  "for i in $(seq 1 150); do /bin/true; done; echo EXEC-$((6*7))", "EXEC-42", 400),
    ("fork-burst", "for r in $(seq 1 6); do for i in $(seq 1 10); do /bin/echo b$r$i > /dev/null & done; wait; done; echo BURST-$((6*7))",
                   "BURST-42", 400),
    ("cmdsubst",   "x=$(for i in $(seq 1 30); do echo $(echo $(echo n$i)); done | wc -l); echo SUBST-$((x+12))", "SUBST-42", 400),
    ("md5-pipe",   "a=$(cat /bin/bash | md5sum); b=$(cat /bin/bash | md5sum); [ \"$a\" = \"$b\" ] && echo MD5-$((6*7))", "MD5-42", 400),
    ("dd-pipe",    "echo DD-$(dd if=/dev/zero bs=4096 count=2000 2>/dev/null | wc -c)", "DD-8192000", 400),
    ("smp-fpu",    "/bin/smptest fpu 12 2000000", "[smptest] fpu PASS", 600),
    ("smp-burst",  "/bin/smptest burst 25 12", "[smptest] burst PASS", 600),
    ("smp-pty",    "/bin/smptest pty 16 32", "[smptest] pty PASS", 600),    # [M51] 16 pty pairs
    ("smp-pipe",   "/bin/smptest pipe 3 3 256", "[smptest] pipe PASS", 600), # [M51] checked pipelines
    ("fuzz-par",   "( /bin/sysfuzz -s 11 -b 4 -c 100 & /bin/sysfuzz -s 13 -b 4 -c 100 & /bin/sysfuzz -s 17 -b 4 -c 100 & /bin/sysfuzz -s 19 -b 4 -c 100 & wait ); echo FUZZ-$((6*7))",
                   "FUZZ-42", 600),
]
QUICK = {"cpu-count", "spin-jobs", "pipelines", "smp-fpu", "smp-burst", "smp-pty", "fuzz-par"}
FAIL_MARKERS = ["[EXC] int=", "[PANIC]", "nested fault", "stack smashing", "CORRUPT",
                "[TLB] BUG", "shootdown timeout", "LOCKDEP"]
# A CPU exception taken in ring 3 is logged "[EXC] int=... ring3", followed by
# "[EXC] ring3 fault -> kill pid=..." and "[KILL]": the process died of its own
# fault and the kernel carried on (M15). Only "ring0" (or a line without the
# ring word) is a kernel fault. Ring-3 kills are allowed only in the steps that
# provoke them: sysfuzz hands its own memory (g_buf, whose end borders the
# fuzzer's globals such as its RNG state) to mprotect, mmap(MAP_FIXED), read
# and friends, so a batch child may die of it; sysfuzz itself reports that as
# fine ("child KILLED ... repro").
USER_KILL_OK = {"fuzz-par"}

def exc_lines(d):
    return [l for l in d.splitlines() if "[EXC] int=" in l]

def is_user_exc(line):
    return line.rstrip().endswith(" ring3")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", default="1G")
    ap.add_argument("--log", default="/tmp/secos_smp_stress.log")
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--cpu", default=None)
    ap.add_argument("--boot", type=float, default=240)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--cpus-only", action="store_true")
    ap.add_argument("--expect-cpus", type=int, default=None)
    o = ap.parse_args()
    ncpu = o.expect_cpus if o.expect_cpus is not None else o.smp
    cpulist = "0" if ncpu == 1 else f"0-{ncpu - 1}"
    steps = [("cpu-count",
              "echo NP-$(nproc)-$(nproc --all)-$(cat /sys/devices/system/cpu/online)"
              "-$(grep Cpus_allowed_list /proc/self/status | cut -f2); /bin/smptest cpus",
              f"NP-{ncpu}-{ncpu}-{cpulist}-{cpulist}", 120)]
    if not o.cpus_only: steps += STEPS

    qemu = [o.qemu, "-cdrom", o.iso, "-boot", "d", "-m", o.mem, "-smp", str(o.smp),
            "-debugcon", "file:" + o.log, "-global", "isa-debugcon.iobase=0xe9",
            "-no-reboot", "-display", "none", "-serial", "stdio"]
    if o.cpu: qemu += ["-cpu", o.cpu]
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
    def logtext():
        try: return open(o.log, "rb").read().decode("latin1")
        except OSError: return ""
    def fault():
        d = logtext()
        for m in FAIL_MARKERS:
            if m == "[EXC] int=":
                if any(not is_user_exc(l) for l in exc_lines(d)): return m
            elif m in d: return m
        return None
    def user_kills(since=0):
        return sum(1 for l in exc_lines(logtext()[since:]) if is_user_exc(l))
    ok = fail = 0
    def report(name, good, why=""):
        nonlocal ok, fail
        if good: ok += 1; print(f"[SSD] {name} OK {why}".rstrip(), flush=True)
        else: fail += 1; print(f"[SSD] {name} FAIL {why}".rstrip(), flush=True)

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
    if not prompt:
        p.kill(); print(f"[SSD] RESULT ok={ok} fail={fail}"); sys.exit(1)
    time.sleep(1.0)

    idx = 0
    for name, cmd, expect, tmo in steps:
        if o.quick and name not in QUICK: continue
        sentinel = f"SSD-END-{idx}"
        line = f"{cmd}; echo SSD-END-$(({idx}+0))\r"
        with lock: since = len(buf)
        logsince = len(logtext())
        p.stdin.write(line.encode()); p.stdin.flush()
        done = wait_for(sentinel, tmo, since)
        with lock: out = bytes(buf[since:]).decode("latin1")
        m = fault()
        nk = user_kills(logsince)
        if not done: report(name, False, f"timeout {tmo}s")
        elif m: report(name, False, f"kernel marker {m!r}")
        elif nk and name not in USER_KILL_OK: report(name, False, f"unexpected ring-3 kill(s): {nk}")
        elif expect not in out: report(name, False, f"expected {expect!r} missing")
        elif name == "cpu-count" and "[smptest] cpus PASS" not in out:
            report(name, False, "smptest cpus FAIL: " + (re.findall(r"\[smptest\] cpus=[^\r\n]*", out) or ["?"])[0])
        else: report(name, True, f"({nk} ring-3 kill(s) of fuzz children, allowed)" if nk else "")
        idx += 1
        if not alive[0]: break

    if o.cpus_only:
        report("no-kernel-fault", fault() is None, str(fault() or ""))
        p.kill()
        print(f"[SSD] RESULT ok={ok} fail={fail}", flush=True)
        sys.exit(0 if fail == 0 else 1)

    # /proc/smp: work spread across CPUs, no shootdown timeouts
    with lock: since = len(buf)
    p.stdin.write(f"cat /proc/smp; echo SSD-END-{idx}-$((0))\r".encode()); p.stdin.flush()
    wait_for(f"SSD-END-{idx}-0", 60, since)
    with lock: out = bytes(buf[since:]).decode("latin1")
    mig = sum(int(x) for x in re.findall(r"migrations (\d+)", out))
    rs = sum(int(x) for x in re.findall(r"ipi_resched (\d+)", out))
    tmo = re.search(r"shootdown_timeouts (\d+)", out)
    online = re.search(r"online (\d+)", out)
    report("proc-smp-online", bool(online) and int(online.group(1)) == o.smp,
           f"online={online.group(1) if online else '?'}")
    if o.smp > 1:
        report("proc-smp-migrations", mig > 0, f"migrations={mig}")
        report("proc-smp-resched-ipis", rs > 0, f"ipi_resched={rs}")
    report("proc-smp-no-shootdown-timeout", bool(tmo) and tmo.group(1) == "0")
    report("no-kernel-fault", fault() is None, str(fault() or ""))
    p.kill()
    print(f"[SSD] RESULT ok={ok} fail={fail}", flush=True)
    sys.exit(0 if fail == 0 else 1)

if __name__ == "__main__":
    main()
