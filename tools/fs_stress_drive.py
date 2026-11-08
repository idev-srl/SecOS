#!/usr/bin/env python3
# tools/fs_stress_drive.py — filesystem stress session (M51 C3, plan §7 tests 1+2).
# SPDX-License-Identifier: MIT
#
# Boots a SecOS ISO built with CONSOLE=root in QEMU with -smp N on a fresh
# persistent ext4 root (tools/mkroot.sh, virtio vda) and drives /bin/fsstress
# over the serial console:
#
#   default:  fsstress <target> <workers> <iters> <seed> must PASS (N forked
#             workers: create/write pattern, O_APPEND to a shared log, rename,
#             unlink, mkdir/rmdir, truncate, getdents, stat, symlink/link/
#             chmod, mmap-read vs read; in-guest verification of every file
#             and of the log), --rounds times with different seeds; then a
#             clean `poweroff` and the host's `e2fsck -fn` must be clean;
#   --crash K: K times: start a long fsstress, kill -9 QEMU at a random point
#             while it runs, boot again (the kernel replays the journal),
#             `fsstress -c` on every leftover run directory must find the log
#             intact (whole lines, gap-free per worker), remove the leftovers,
#             poweroff, and `e2fsck -fn` must be clean after every iteration.
#
# The target directory is a parameter (--target, default /root) so the same
# driver aims fsstress at another mount (ramfs: --target /tmp).
# [M52] --data vfat|exfat moves the root to SATA (q35 AHCI, sda) and adds a
# data disk (sdb) with a fresh
# FAT32 / exFAT volume, mounts it at /mnt, runs fsstress -n there (no
# symlinks / hard links / modes) and checks it on the host after the
# poweroff: fsck.vfat -n, or fsck.exfat -n + tools/exfat_img.py deepcheck.
# Every round prints its wall time and ops/s (workers * iters / time).
# The debugcon log must stay free of kernel fault and lockdep markers.
#
# Output: one "[FSD] <step> OK|FAIL ..." line per step and "[FSD] RESULT ok=N
# fail=M". Exit 0 when every step passed. Everything lives in --work (a fresh
# directory); host ports are never used (no network forwarding).
#
# Usage: fs_stress_drive.py --iso ISO [--smp 4] [--mem 1G] [--work DIR]
#                           [--target /root] [--workers 6] [--iters 150]
#                           [--rounds 2] [--crash K] [--seed S]
#                           [--qemu BIN] [--qemu-pre ARGS] [--boot SECS]
import sys, os, time, subprocess, threading, re, argparse, random, shutil, shlex

FAIL_MARKERS = ["[EXC] int=", "[PANIC]", "nested fault", "stack smashing", "CORRUPT",
                "[TLB] BUG", "shootdown timeout", "LOCKDEP]", "[LOCK] BUG"]
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class VM:
    def __init__(self, o, disk, log, data=None):
        self.log = log
        open(log, "w").close()
        # With a data disk both disks are SATA (q35 AHCI: root sda, data sdb):
        # the kernel drives one virtio disk and probes AHCI only without it.
        if data:
            disks = ["-machine", "q35", "-drive", f"file={disk},if=ide,index=0,format=raw",
                     "-drive", f"file={data},if=ide,index=1,format=raw"]
        else:
            disks = [] if o.ramroot else ["-drive", f"file={disk},if=virtio,format=raw"]
        cmd = [o.qemu] + shlex.split(o.qemu_pre) + ["-cdrom", o.iso, "-boot", "d", "-m", o.mem, "-smp", str(o.smp)] + disks + [
               "-nic", "none",
               "-debugcon", "file:" + log, "-global", "isa-debugcon.iobase=0xe9",
               "-no-reboot", "-display", "none", "-serial", "stdio"]
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.buf = bytearray(); self.lock = threading.Lock(); self.alive = True
        threading.Thread(target=self._rd, daemon=True).start()

    def _rd(self):
        while True:
            b = self.p.stdout.read1(65536)
            if not b: self.alive = False; break
            with self.lock: self.buf.extend(b)

    def mark(self):
        with self.lock: return len(self.buf)

    def out(self, since=0):
        with self.lock: return bytes(self.buf[since:]).decode("latin1")

    def wait_for(self, needle, timeout, since=0):
        nb = needle.encode(); t0 = time.time()
        while time.time() - t0 < timeout:
            with self.lock:
                if nb in self.buf[since:]: return True
            if not self.alive: return False
            time.sleep(0.05)
        return False

    def wait_prompt(self, timeout):
        # [M52] init's console messages ("[init] gui ... exited") can land
        # right after bash's first prompt (they run in parallel now), and
        # then the tail never ends with '#': once bash has printed a prompt,
        # ask for a fresh one every few seconds.
        t0 = time.time(); poke = 0.0
        while time.time() - t0 < timeout and self.alive:
            with self.lock: tail = bytes(self.buf[-64:]); seen = b"bash-" in self.buf
            if tail.rstrip().endswith(b"#"): return True
            if seen and time.time() - poke > 5.0:
                poke = time.time()
                self.p.stdin.write(b"\r"); self.p.stdin.flush()
            time.sleep(0.2)
        return False

    def send(self, line):
        self.p.stdin.write((line + "\r").encode()); self.p.stdin.flush()

    def dlog(self):
        try: return open(self.log, "rb").read().decode("latin1")
        except OSError: return ""

    def fault(self):
        d = self.dlog()
        for m in FAIL_MARKERS:
            if m in d: return m
        for l in d.splitlines():
            if "HEAP: " in l and "(expected)" not in l: return l.strip()[:120]
        return None

    def wait_exit(self, timeout):
        try: self.p.wait(timeout); return True
        except subprocess.TimeoutExpired: return False

    def kill(self):
        try: self.p.kill()
        except OSError: pass
        try: self.p.wait(10)
        except subprocess.TimeoutExpired: pass


def fsck_clean(img, out):
    """e2fsck -fn: exit 0 and nothing printed besides the pass headers."""
    r = subprocess.run(["e2fsck", "-fn", img], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    txt = r.stdout.decode(errors="replace")
    open(out, "w").write(txt)
    extra = [l for l in txt.splitlines()
             if l.strip() and not re.match(r"^(e2fsck [0-9]|Pass [1-5]|SECOSROOT: [0-9]+/[0-9]+ files)", l)]
    return r.returncode == 0 and not extra, (txt.strip().splitlines() or ["?"])[-1] if extra or r.returncode else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", default="1G")
    ap.add_argument("--work", default=None)
    ap.add_argument("--target", default="/root")
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--iters", type=int, default=150)
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--crash", type=int, default=0)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    # [M54] Extra QEMU arguments placed first (e.g. "-device intel-iommu",
    # which must come before any PCI device).
    ap.add_argument("--qemu-pre", default="")
    ap.add_argument("--boot", type=float, default=300)
    ap.add_argument("--data", choices=["vfat", "exfat"], default=None)
    ap.add_argument("--mount-race", type=int, default=0)
    # [M52] No disk: the root is ramfs (on a persistent root /tmp is just a
    # directory of the ext4 volume), fsstress runs on it, no fsck.
    ap.add_argument("--ramroot", action="store_true")
    o = ap.parse_args()
    seed = o.seed if o.seed is not None else int(time.time()) % 100000
    rng = random.Random(seed)
    work = o.work or os.path.join("/tmp", f"secos_fsd.{os.getpid()}")
    os.makedirs(work, exist_ok=True)
    img = os.path.join(work, "root.img")
    data = os.path.join(work, "data.img") if o.data else None
    if o.data:
        if o.target == "/root": o.target = "/mnt"
        if os.path.exists(data): os.unlink(data)
        if o.data == "vfat":
            subprocess.run(["truncate", "-s", "512M", data])
            dr = subprocess.run(["mkfs.fat", "-F", "32", "-n", "FSD", data],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        else:
            dr = subprocess.run(["bash", os.path.join(REPO, "tools", "exfat-host.sh"), "mkfs", data, "512"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if os.path.exists(img): os.unlink(img)
    r = subprocess.run(["bash", os.path.join(REPO, "tools", "mkroot.sh"), img, "1G"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    ok = fail = 0
    def report(name, good, why=""):
        nonlocal ok, fail
        if good: ok += 1; print(f"[FSD] {name} OK {why}".rstrip(), flush=True)
        else: fail += 1; print(f"[FSD] {name} FAIL {why}".rstrip(), flush=True)
    def finish():
        print(f"[FSD] RESULT ok={ok} fail={fail} seed={seed} work={work}", flush=True)
        sys.exit(0 if fail == 0 else 1)
    report("mkroot", r.returncode == 0 and os.path.exists(img), r.stdout.decode(errors="replace")[-200:])
    if o.data:
        report(f"mkfs-{o.data}", dr.returncode == 0 and os.path.exists(data), dr.stdout.decode(errors="replace")[-200:])
    if fail: finish()
    print(f"[FSD] seed={seed} smp={o.smp} target={o.target} workers={o.workers} iters={o.iters}", flush=True)

    idx = [0]
    def run(vm, name, cmd, expect, tmo, absent=("FAIL",)):
        sentinel = f"FSD-END-{idx[0]}"
        line = f"{cmd}; echo FSD-END-$(({idx[0]}+0))"
        since = vm.mark(); t0 = time.time()
        vm.send(line)
        done = vm.wait_for(sentinel, tmo, since)
        el = f"({time.time() - t0:.0f}s)"
        out = vm.out(since)
        # The echoed command line itself contains the expected text for some
        # checks: only count output after the first newline.
        body = out.split("\n", 1)[1] if "\n" in out else ""
        m = vm.fault()
        miss = [e for e in expect if e not in body]
        extra = [a for a in absent if a in body]
        if not done: report(name, False, f"timeout {tmo}s")
        elif m: report(name, False, f"kernel marker {m!r}")
        elif miss or extra:
            fl = re.findall(r"\[fsstress\][^\r\n]*FAIL[^\r\n]*", body)
            report(name, False, f"missing {miss!r} unexpected {extra!r} {' | '.join(fl)[:400]}")
        else: report(name, True, el)
        idx[0] += 1
        return done and not m and not miss and not extra, body

    def boot(tag):
        vm = VM(o, img, os.path.join(work, f"{tag}.log"), data)
        good = vm.wait_prompt(o.boot)
        report(f"{tag}-boot-prompt", good)
        if good: time.sleep(1.0)
        return vm, good

    def poweroff(vm, tag):
        vm.send("sync; poweroff")
        good = vm.wait_exit(120)
        if not good: vm.kill()
        report(f"{tag}-poweroff", good)
        m = vm.fault()
        report(f"{tag}-no-kernel-fault", m is None, str(m or ""))

    def fsck(tag):
        good, why = fsck_clean(img, os.path.join(work, f"{tag}.fsck"))
        report(f"{tag}-e2fsck-clean", good, why)

    if o.crash == 0:
        vm, good = boot("run")
        if not good: vm.kill(); finish()
        if o.ramroot:
            run(vm, "root-is-ramfs", "grep -q ' / ramfs ' /proc/mounts && echo ROOT-$((6*7))", ["ROOT-42"], 60, absent=())
        else:
            run(vm, "root-is-ext4", "grep -q ' / ext4 ' /proc/mounts && echo ROOT-$((6*7))", ["ROOT-42"], 60, absent=())
        nopt = "-n " if o.ramroot else ""   # ramfs: no symlinks / hard links
        if o.data:
            fst = "vfat" if o.data == "vfat" else "exfat"
            run(vm, f"mount-{o.data}",
                f"(grep -q ' /mnt ' /proc/mounts || mount -t {fst} /dev/sdb /mnt) && "
                f"grep ' /mnt ' /proc/mounts && echo MNT-$((6*7))", ["MNT-42"], 120, absent=())
            nopt = "-n "
        if o.data and o.mount_race:
            # [M52] Plan test 5: umount racing opens, reads, writes and
            # listings on other CPUs. Each umount must succeed or fail with
            # EBUSY/EINVAL, never fault; the volume stays fsck-clean (below).
            fst = "vfat" if o.data == "vfat" else "exfat"
            n = o.mount_race
            run(vm, "mount-race",
                f"mkdir -p /mnt; k=0; for i in $(seq 1 {n}); do "
                f"grep -q ' /mnt ' /proc/mounts || mount -t {fst} /dev/sdb /mnt 2>/dev/null; "
                f"( for j in 1 2 3 4; do echo r$i$j > /mnt/mr$j 2>/dev/null; cat /mnt/mr$j >/dev/null 2>&1; "
                f"ls /mnt >/dev/null 2>&1; done ) & ( exec 3</mnt/mr1 2>/dev/null; sleep 0.0$((i%7)); exec 3<&- ) & "
                f"umount /mnt 2>/dev/null && k=$((k+1)); wait; done; "
                f"grep -q ' /mnt ' /proc/mounts || mount -t {fst} /dev/sdb /mnt; rm -f /mnt/mr*; "
                f"grep -q ' /mnt ' /proc/mounts && echo MR-$((6*7))-umounts=$k", ["MR-42"], 900, absent=())
        for k in range(o.rounds):
            s = seed + k
            t0 = time.time()
            good, _ = run(vm, f"fsstress-{k}", f"/bin/fsstress {nopt}{o.target} {o.workers} {o.iters} {s}",
                          [f"[fsstress] {o.target}/fss.", " PASS workers="], 3000)
            el = time.time() - t0
            if good:
                print(f"[FSD] perf round={k} smp={o.smp} target={o.target} workers={o.workers} "
                      f"iters={o.iters} secs={el:.1f} ops/s={o.workers * o.iters / el:.1f}", flush=True)
        run(vm, "no-leftovers", f"n=$(ls {o.target} | grep -c '^fss[.]'); echo LEFT-$n", ["LEFT-0"], 60, absent=())
        if o.data:
            run(vm, "umount-data", "sync; umount /mnt && echo UMNT-$((6*7))", ["UMNT-42"], 120, absent=())
        poweroff(vm, "run")
        if not o.ramroot: fsck("run")
        if o.data == "vfat":
            fr = subprocess.run(["fsck.vfat", "-n", data], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            report("data-fsck.vfat-clean", fr.returncode == 0, fr.stdout.decode(errors="replace").strip()[-300:])
        elif o.data == "exfat":
            fr = subprocess.run(["bash", os.path.join(REPO, "tools", "exfat-host.sh"), "fsck", data],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            report("data-fsck.exfat-clean", fr.returncode == 0, fr.stdout.decode(errors="replace").strip()[-300:])
            dc = subprocess.run(["python3", "-I", os.path.join(REPO, "tools", "exfat_img.py"), "deepcheck", data],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            report("data-exfat-deepcheck-clean", dc.returncode == 0, dc.stdout.decode(errors="replace").strip()[-300:])
        finish()

    # ---- crash-consistency iterations ----
    for it in range(o.crash):
        tag = f"crash{it}"
        vm, good = boot(tag)
        if not good: vm.kill(); finish()
        since = vm.mark()
        vm.send(f"/bin/fsstress -k {o.target} {o.workers} 100000 {seed + it}")
        started = vm.wait_for("[fsstress] start", 60, since)
        report(f"{tag}-started", started)
        delay = rng.uniform(4.0, 25.0)
        time.sleep(delay)
        m = vm.fault()
        alive = vm.alive
        vm.kill()
        report(f"{tag}-killed-mid-run", alive and m is None and "PASS" not in vm.out(since) and
               "FAIL" not in vm.out(since), f"after {delay:.1f}s {m or ''}")
        # The host's own recovery must agree: e2fsck -fy on a copy may replay
        # the journal and must leave nothing else to fix.
        hc = img + ".host"
        subprocess.run(["cp", "--sparse=always", img, hc])
        r = subprocess.run(["e2fsck", "-fy", hc], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        hrep = "journal replayed" if b"recovering journal" in r.stdout else "journal clean"
        good, why = fsck_clean(hc, os.path.join(work, f"{tag}.hostfsck"))
        fixed = re.search(rb"Fix\?|Clear\?|Salvage\?|Recreate\?|Relocate\?", r.stdout)
        report(f"{tag}-host-replay-clean", r.returncode in (0, 1) and not fixed and good,
               f"{hrep} {why} {'(e2fsck -fy had to fix something)' if fixed else ''}".strip())
        os.unlink(hc)
        vm, good = boot(f"{tag}-replay")
        if not good: vm.kill(); finish()
        d = vm.dlog()
        rec = re.search(r"\[M27\] journal recover: txns=0x([0-9A-Fa-f]+)", d)
        print(f"[FSD] {tag} kernel journal replay: " +
              (f"txns={int(rec.group(1), 16)}" if rec else "journal was empty"), flush=True)
        run(vm, f"{tag}-root-is-ext4", "grep -q ' / ext4 ' /proc/mounts && echo ROOT-$((6*7))", ["ROOT-42"], 60, absent=())
        run(vm, f"{tag}-log-intact",
            f"for b in {o.target}/fss.*; do /bin/fsstress -c $b; done",
            ["[fsstress] check ", " PASS lines="], 600)
        run(vm, f"{tag}-cleanup", f"rm -rf {o.target}/fss.* && echo CLEAN-$((6*7))", ["CLEAN-42"], 600, absent=())
        poweroff(vm, f"{tag}-replay")
        fsck(tag)
        if fail: break
    finish()


if __name__ == "__main__":
    main()
