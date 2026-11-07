#!/usr/bin/env python3
# tools/pc_stress_drive.py — page-cache coherence stress session (M50 B2).
# SPDX-License-Identifier: MIT
#
# Boots a SecOS ISO built with CONSOLE=root (optionally with
# -DSECOS_PAGECACHE_TEST) in QEMU with -smp N and a fresh default mkfs.ext4
# data disk on virtio (mounted at /mnt), then:
#   - pctest:   (--pctest) the boot self-test of the lock-free fill (forced
#               races on the BSP and against an AP) passed: "[PCTEST] DONE"
#               with fail=0 and the expected number of checks;
#   - mnt-ext4: /mnt is the ext4 disk;
#   - pcstress: /bin/pcstress on the ramfs /tmp and on the ext4 /mnt at the
#               same time (writers overwrite / O_TRUNC / O_APPEND / ftruncate
#               shrink+grow while hammers read() and mmap the same files and
#               lock-step checkers verify every completed write through
#               read() and a fresh mmap) — both must PASS;
#   - pcstress-{mnt,tmp}-heavy: one directory at a time with more checkers
#               and hammers;
#   - umount:   /mnt unmounts cleanly;
# and finally the host runs `e2fsck -fn` on the disk, and the debugcon log
# must be free of kernel fault and lockdep markers.
#
# Output: one "[PCD] <step> OK|FAIL ..." line per step and "[PCD] RESULT ok=N
# fail=M". Exit 0 when every step passed.
#
# Usage: pc_stress_drive.py --iso ISO [--smp 4] [--mem 1G] [--log LOG]
#                           [--disk IMG] [--rounds 120] [--pctest]
#                           [--qemu BIN] [--boot SECS]
import sys, time, subprocess, threading, re, argparse, os

FAIL_MARKERS = ["[EXC] int=", "[PANIC]", "nested fault", "stack smashing", "CORRUPT",
                "[TLB] BUG", "shootdown timeout", "LOCKDEP]", "[LOCK] BUG"]
PCTEST_CHECKS = 23      # mm/pagecache_test.c with >= 2 CPUs online

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", default="1G")
    ap.add_argument("--log", default="/tmp/secos_pc_stress.log")
    ap.add_argument("--disk", default=None, help="ext4 image to create (default: next to --log)")
    ap.add_argument("--rounds", type=int, default=120)
    ap.add_argument("--pctest", action="store_true")
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--boot", type=float, default=240)
    o = ap.parse_args()
    disk = o.disk or (os.path.splitext(o.log)[0] + ".ext4.img")

    ok = fail = 0
    def report(name, good, why=""):
        nonlocal ok, fail
        if good: ok += 1; print(f"[PCD] {name} OK {why}".rstrip(), flush=True)
        else: fail += 1; print(f"[PCD] {name} FAIL {why}".rstrip(), flush=True)

    if os.path.exists(disk): os.unlink(disk)
    with open(disk, "wb") as f: f.truncate(64 << 20)
    r = subprocess.run(["mkfs.ext4", "-q", "-F", "-L", "PCSDATA", disk],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        report("mkfs.ext4", False, r.stdout.decode(errors="replace")[:200])
        print(f"[PCD] RESULT ok={ok} fail={fail}"); sys.exit(1)

    qemu = [o.qemu, "-cdrom", o.iso, "-boot", "d", "-m", o.mem, "-smp", str(o.smp),
            "-drive", f"file={disk},if=virtio,format=raw",
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
        # Heap corruption reports; the boot heap self-test prints its own
        # negative controls as "[HEAP] (expected) HEAP: ...".
        for l in d.splitlines():
            if "HEAP: " in l and "(expected)" not in l: return l.strip()[:120]
        return None

    t0 = time.time(); prompt = False
    while time.time() - t0 < o.boot and alive[0]:
        with lock: tail = bytes(buf[-64:])
        if tail.rstrip().endswith(b"#"): prompt = True; break
        time.sleep(0.2)
    report("boot-prompt", prompt)
    if not prompt:
        p.kill(); print(f"[PCD] RESULT ok={ok} fail={fail}"); sys.exit(1)
    time.sleep(1.0)

    if o.pctest:
        d = dlog()
        m = re.search(r"\[PCTEST\] DONE ok=0x([0-9A-Fa-f]+) fail=0x([0-9A-Fa-f]+)", d)
        nok = int(m.group(1), 16) if m else -1
        nbad = int(m.group(2), 16) if m else -1
        bad = re.findall(r"\[PCTEST\] [^\n]*: FAIL", d)
        report("pctest", bool(m) and nbad == 0 and nok == PCTEST_CHECKS,
               f"ok={nok} fail={nbad} {' | '.join(bad)[:300]}")
        report("pctest-cross-cpu", "[PCTEST] AP: waiter gets the published page: OK" in d
               and "cross-CPU cases skipped" not in d)

    idx = [0]
    def run(name, cmd, expect, tmo, absent=()):
        sentinel = f"PCD-END-{idx[0]}"
        line = f"{cmd}; echo PCD-END-$(({idx[0]}+0))\r"
        with lock: since = len(buf)
        t_start = time.time()
        p.stdin.write(line.encode()); p.stdin.flush()
        done = wait_for(sentinel, tmo, since)
        el = f"({time.time() - t_start:.0f}s)"
        with lock: out = bytes(buf[since:]).decode("latin1")
        m = fault()
        miss = [e for e in expect if e not in out]
        extra = [a for a in absent if a in out]
        if not done: report(name, False, f"timeout {tmo}s")
        elif m: report(name, False, f"kernel marker {m!r}")
        elif miss or extra:
            fl = re.findall(r"\[pcstress\][^\r\n]*FAIL[^\r\n]*", out)
            report(name, False, f"missing {miss!r} unexpected {extra!r} {' | '.join(fl)[:300]}")
        else: report(name, True, el)
        idx[0] += 1
        return out

    run("mnt-ext4", "grep -q ' /mnt ext4 ' /proc/mounts && echo MNT-$((6*7))", ["MNT-42"], 60)
    n = o.rounds
    # The process table has 32 slots (init, bash and the shell's subshells
    # included): the parallel run uses 2 x 9 processes, the heavier runs
    # 15 each, one at a time.
    run("pcstress", f"( /bin/pcstress /tmp {n} 1 2 & /bin/pcstress /mnt {n} 1 2 & wait )",
        ["[pcstress] /tmp PASS", "[pcstress] /mnt PASS"], 1200, absent=["FAIL"])
    run("pcstress-mnt-heavy", f"/bin/pcstress /mnt {n} 2 4",
        ["[pcstress] /mnt PASS"], 1200, absent=["FAIL"])
    run("pcstress-tmp-heavy", f"/bin/pcstress /tmp {n} 2 4",
        ["[pcstress] /tmp PASS"], 1200, absent=["FAIL"])
    run("umount", "sync; umount /mnt && echo UMOUNT-$((6*7))", ["UMOUNT-42"], 120)
    report("no-kernel-fault", fault() is None, str(fault() or ""))
    p.kill(); p.wait()
    r = subprocess.run(["e2fsck", "-fn", disk], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    report("host-e2fsck", r.returncode == 0, r.stdout.decode(errors="replace").strip().splitlines()[-1:][0][:200]
           if r.stdout else "")
    print(f"[PCD] RESULT ok={ok} fail={fail}", flush=True)
    sys.exit(0 if fail == 0 else 1)

if __name__ == "__main__":
    main()
