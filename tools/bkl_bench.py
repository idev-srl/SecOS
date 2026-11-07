#!/usr/bin/env python3
# tools/bkl_bench.py — M52 scaling benchmark (plan §7.7): boots a SecOS ISO
# built with CONSOLE=root on a fresh ext4 root (tools/mkroot.sh) with -smp N
# and an explicit kernel lock mode, then measures over the serial console:
#   exec      4 parallel loops of `bash -c true` (execs/s);
#   pipe      4 parallel loops of `seq 1 2000 | cat | wc -l` (pipelines/s);
#   fsstress  /bin/fsstress on the ext4 root, W workers x I iters (iters/s);
#   ramfs     the same fsstress on /tmp (ramfs);
# and prints /proc/lockstat at the end (the most contended locks). Every
# number is wall time measured in the guest (/proc/uptime). The boot word is
# chosen with a GRUB entry rewrite of a copy of the ISO tree, so one build
# serves both modes.
#
# Output: "[BENCH] <name> mode=.. smp=.. value=.. unit=.." lines, the lockstat
# top, "[BENCH] RESULT ok=N fail=M". Never binds a host port.
#
# Usage: bkl_bench.py --iso ISO --mode on|off [--smp 4] [--work DIR]
#                     [--execs 200] [--pipes 100] [--workers 6] [--iters 100]
import argparse, os, re, shutil, subprocess, sys, threading, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAIL_MARKERS = ["[EXC] int=", "[PANIC]", "[LOCKDEP]", "[LOCK] BUG", "CORRUPTION"]


def mode_iso(iso, mode, work):
    """Copy of ISO whose default GRUB entry boots with bkl=<mode>."""
    tree = os.path.join(work, "isotree")
    out = os.path.join(work, f"bench-{mode}.iso")
    shutil.rmtree(tree, ignore_errors=True)
    subprocess.run(["xorriso", "-osirrox", "on", "-indev", iso, "-extract", "/", tree],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    subprocess.run(["chmod", "-R", "u+w", tree], check=True)
    cfg = os.path.join(tree, "boot", "grub", "grub.cfg")
    s = open(cfg).read()
    s = re.sub(r"^set timeout=\d+", "set timeout=0", s, flags=re.M)
    s = s.replace("    multiboot2 /boot/kernel.bin\n", f"    multiboot2 /boot/kernel.bin bkl={mode}\n", 1)
    open(cfg, "w").write(s)
    subprocess.run(["grub-mkrescue", "--output=" + out, tree],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--mode", choices=["on", "off"], required=True)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", default="1G")
    ap.add_argument("--work", default=None)
    ap.add_argument("--execs", type=int, default=200)
    ap.add_argument("--pipes", type=int, default=100)
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--iters", type=int, default=100)
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    o = ap.parse_args()
    work = o.work or os.path.join("/tmp", f"secos_bench.{os.getpid()}")
    os.makedirs(work, exist_ok=True)
    iso = mode_iso(o.iso, o.mode, work)
    img = os.path.join(work, "root.img")
    if os.path.exists(img): os.unlink(img)
    subprocess.run(["bash", os.path.join(REPO, "tools", "mkroot.sh"), img, "1G"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    log = os.path.join(work, "debugcon.log")
    open(log, "w").close()
    p = subprocess.Popen([o.qemu, "-cdrom", iso, "-boot", "d", "-m", o.mem, "-smp", str(o.smp),
                          "-drive", f"file={img},if=virtio,format=raw", "-nic", "none",
                          "-debugcon", "file:" + log, "-global", "isa-debugcon.iobase=0xe9",
                          "-no-reboot", "-display", "none", "-serial", "stdio"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    buf = bytearray(); lk = threading.Lock()
    def rd():
        while True:
            b = p.stdout.read1(65536)
            if not b: break
            with lk: buf.extend(b)
    threading.Thread(target=rd, daemon=True).start()
    ok = fail = 0
    def report(name, good, why=""):
        nonlocal ok, fail
        if good: ok += 1
        else: fail += 1
        print(f"[BENCH] {name} {'OK' if good else 'FAIL'} {why}".rstrip(), flush=True)
    def fault():
        d = open(log, "rb").read().decode("latin1")
        for m in FAIL_MARKERS:
            if m in d: return m
        return None
    idx = [0]
    def run(cmd, tmo):
        sentinel = f"BN-END-{idx[0]}"
        line = f"{cmd}; echo BN-END-$(({idx[0]}+0))\r"
        idx[0] += 1
        with lk: since = len(buf)
        p.stdin.write(line.encode()); p.stdin.flush()
        t0 = time.time()
        while time.time() - t0 < tmo:
            with lk:
                if sentinel.encode() in buf[since:]: break
            if p.poll() is not None: break
            time.sleep(0.1)
        with lk: out = bytes(buf[since:]).decode("latin1")
        return sentinel in out, out
    t0 = time.time()
    while time.time() - t0 < 300:
        with lk: tail = bytes(buf[-64:]); seen = b"bash-" in buf
        if tail.rstrip().endswith(b"#"): break
        if seen: p.stdin.write(b"\r"); p.stdin.flush(); time.sleep(4.5)   # fresh prompt
        time.sleep(0.5)
    time.sleep(2)
    done, out = run("grep -q ' / ext4 ' /proc/mounts && echo ROOT-$((6*7)); grep -o 'bkl o[nf]*' /proc/smp", 60)
    report("boot", done and "ROOT-42" in out and f"bkl {o.mode}" in out.split("\n", 1)[-1])

    def timed(name, body, n, unit):
        cmd = f"t0=$(cut -d' ' -f1 /proc/uptime); {body}; t1=$(cut -d' ' -f1 /proc/uptime); echo BT-$t0-$t1"
        done, out = run(cmd, 3600)
        tm = re.findall(r"BT-([\d.]+)-([\d.]+)", out)
        secs = float(tm[-1][1]) - float(tm[-1][0]) if done and tm else 0.0
        good = done and secs > 0 and "FAIL" not in out.split("\n", 1)[-1]
        report(name, good, f"secs={secs:.2f}")
        if good:
            print(f"[BENCH] {name} mode={o.mode} smp={o.smp} value={n / secs:.2f} unit={unit} secs={secs:.2f}",
                  flush=True)
    per = max(1, o.execs // 4)
    timed("exec", " ".join(f"( for i in $(seq 1 {per}); do bash -c true; done ) &" for _ in range(4)) + " wait",
          4 * per, "execs/s")
    per = max(1, o.pipes // 4)
    timed("pipe", " ".join(f"( for i in $(seq 1 {per}); do seq 1 2000 | cat | wc -l >/dev/null; done ) &"
                          for _ in range(4)) + " wait", 4 * per, "pipelines/s")
    timed("fsstress", f"/bin/fsstress /root {o.workers} {o.iters} 4242", o.workers * o.iters, "iters/s")
    timed("ramfs", f"/bin/fsstress /tmp {o.workers} {o.iters} 4343", o.workers * o.iters, "iters/s")
    done, out = run("cat /proc/lockstat", 60)
    rows = []
    for line in out.splitlines():
        f = line.split()
        # kind name level acquired contended max_wait_us
        if len(f) >= 6 and f[0] in ("klock", "spin") and all(x.isdigit() for x in f[3:6]):
            rows.append((int(f[4]), " ".join(f)))
    rows.sort(reverse=True)
    for _, line in rows[:8]:
        print(f"[BENCH] lockstat {line}", flush=True)
    report("no-kernel-fault", fault() is None, str(fault() or ""))
    run("sync; poweroff", 60)
    try: p.wait(60)
    except subprocess.TimeoutExpired: p.kill()
    print(f"[BENCH] RESULT ok={ok} fail={fail}", flush=True)
    sys.exit(0 if fail == 0 else 1)


if __name__ == "__main__":
    main()
