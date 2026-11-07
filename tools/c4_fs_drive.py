#!/usr/bin/env python3
# tools/c4_fs_drive.py — FAT32 / exFAT / NTFS driver-lock stress (M51 C4).
# SPDX-License-Identifier: MIT
#
# Boots an ISO built with -DSECOS_C4_FSSTRESS (best with -DSECOS_LOCKDEP) on
# q35 with three AHCI disks:
#   sda  fresh 64 MiB FAT32 (mkfs.fat -F 32)
#   sdb  fresh 64 MiB exFAT (tools/exfat-host.sh mkfs)
#   sdc  the M42 NTFS image (make disk-ntfs), with its ntfs-3g listing
# fs/c4_fsstress.c runs file/append/shared-directory/scan jobs on FAT32 and
# exFAT and NTFS readers on two instances, on every CPU at once outside the
# BKL, verifies everything itself and unmounts. Then the host checks:
#   - the guest's verdict and a clean debugcon (no lockdep/lock/heap/CPU faults);
#   - fsck.vfat -n on sda, fsck.exfat -n + exfat_img.py deepcheck + VolumeDirty
#     clear on sdb;
#   - every file the guest left (FILES survivors, shared-directory survivors,
#     LOG.TXT) read back on the host (mtools / exfat-fuse) against the same
#     pattern formula: exact sizes and bytes, every log line once and in order;
#   - the guest's NTFS listing ("[C4N] F|E ...", what 6 concurrent readers
#     agreed on) against the ntfs-3g reference listing.
# Output: "[C4D] <step> OK|FAIL ..." and "[C4D] RESULT ok=N fail=M"; exit 0
# when every step passed.
#
# Usage: c4_fs_drive.py --iso ISO --ntfs NTFS.img [--smp 4] [--out DIR]
import argparse, os, re, shutil, subprocess, sys, tempfile, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAIL_MARKERS = ["[EXC] int=", "PANIC", "nested fault", "stack smashing",
                "[LOCKDEP]", "[LOCK] BUG", "shootdown timeout", "CORRUPTION"]
HEAP_BAD = re.compile(r"\[HEAP\] (?!\(expected\))[^\n]*HEAP:")

# --- the guest's formulas (fs/c4_fsstress.c) ---
M64 = (1 << 64) - 1
def mix64(z):
    z = (z + 0x9E3779B97F4A7C15) & M64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return z ^ (z >> 31)
def pattern(tag, n):
    raw = b"".join(mix64((tag << 48) ^ w).to_bytes(8, "little") for w in range((n + 7) >> 3))
    return raw[:n]
NFILES, NFJOBS, NAPPEND, LOG_LINES, LINE, NSHARED, SHR_K = 6, 8, 4, 150, 32, 4, 20
def files_tag(fs, w, i): return 0x100 + fs * 0x80 + w * 8 + i
def files_size(w, i): return 1000 + (w * 7919 + i * 3271) % 30000
def files_final(w, i): return files_size(w, i) // 2 if i & 1 else files_size(w, i)
def shr_tag(fs, w, k): return 0x800 + fs * 0x100 + w * 0x20 + k
def mkline(fs, w, k):
    l = bytearray(b"." * LINE)
    l[0:12] = f"A{w:02d} L{k:04d} {'X' if fs else 'F'} ".encode()
    l[LINE - 1] = 0x0A
    return bytes(l)

def expected_tree(fs):
    """{relative path (upper case): bytes} of every file the guest leaves."""
    t = {}
    for w in range(NFJOBS):
        for i in range(NFILES):
            if i % 3 == 0: continue
            t[f"D{w:02d}/G{i:02d}.DAT"] = pattern(files_tag(fs, w, i), files_final(w, i))
    for w in range(NSHARED):
        for k in range(0, SHR_K, 2):
            t[f"SHR/S{w:02d}R{k:02d}.DAT"] = pattern(shr_tag(fs, w, k), 100 + k * 97)
    return t

def check_tree(root, fs, report, name):
    want = expected_tree(fs)
    got = {}
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            got[os.path.relpath(p, root).upper()] = open(p, "rb").read()
    bad = [p for p, data in want.items() if got.get(p) != data]
    # SECOS_W.TXT: the boot-time M10 write test of whatever sits on /mnt first.
    extra = [p for p in got if p not in want and p not in ("LOG.TXT", "SECOS_W.TXT")]
    report(f"{name} host readback of {len(want)} files", not bad and not extra,
           f"bad={bad[:4]} extra={extra[:4]}")
    log = got.get("LOG.TXT", b"")
    lines = [log[i:i + LINE] for i in range(0, len(log), LINE)]
    nxt = [0] * NAPPEND; ok = len(log) == NAPPEND * LOG_LINES * LINE
    for l in lines if ok else []:
        m = re.match(rb"A(\d\d) L(\d{4}) ", l)
        if not m: ok = False; break
        w, k = int(m.group(1)), int(m.group(2))
        if w >= NAPPEND or l != mkline(fs, w, k) or k != nxt[w]: ok = False; break
        nxt[w] += 1
    report(f"{name} host LOG.TXT: {len(lines)} whole lines in writer order", ok, f"size={len(log)}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--ntfs", required=True, help="NTFS image from make disk-ntfs (+ .list)")
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", default="1G")
    ap.add_argument("--out", default=None)
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--expect-fail", action="store_true", help="negative control: the guest must FAIL")
    o = ap.parse_args()
    out = o.out or tempfile.mkdtemp(prefix="secos_c4d.")
    os.makedirs(out, exist_ok=True)
    log = os.path.join(out, "debugcon.log")
    sda, sdb, sdc = (os.path.join(out, n) for n in ("sda.img", "sdb.img", "sdc.img"))
    ok = fail = 0
    def report(name, good, why=""):
        nonlocal ok, fail
        if good: ok += 1
        else: fail += 1
        print(f"[C4D] {name} {'OK' if good else 'FAIL'} {'' if good else why}".rstrip(), flush=True)
    def finish():
        print(f"[C4D] RESULT ok={ok} fail={fail} (files in {out})", flush=True)
        sys.exit(0 if fail == 0 else 1)

    with open(sda, "wb") as f: f.truncate(64 << 20)
    subprocess.run(["mkfs.fat", "-F", "32", "-n", "C4FAT", sda], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["bash", f"{REPO}/tools/exfat-host.sh", "mkfs", sdb, "64"], check=True)
    shutil.copyfile(o.ntfs, sdc)

    qemu = [o.qemu, "-machine", "q35", "-m", o.mem, "-smp", str(o.smp),
            "-cdrom", o.iso, "-boot", "d",
            "-drive", f"file={sda},if=ide,index=0,format=raw",
            "-drive", f"file={sdb},if=ide,index=1,format=raw",
            "-drive", f"file={sdc},if=ide,index=3,format=raw",
            "-debugcon", "file:" + log, "-global", "isa-debugcon.iobase=0xe9",
            "-no-reboot", "-display", "none", "-serial", "file:" + os.path.join(out, "serial.log")]
    open(log, "w").close()
    p = subprocess.Popen(qemu, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    def text():
        try: return open(log, "rb").read().decode("utf-8", "replace")
        except OSError: return ""
    t0 = time.time(); done = False
    while time.time() - t0 < o.timeout:
        d = text()
        if "[C4] RESULT" in d or "[C4] SKIP" in d: done = True; break
        if p.poll() is not None: break
        if any(m in d for m in FAIL_MARKERS): time.sleep(2); break
        time.sleep(0.5)
    time.sleep(1.0)
    p.kill(); p.wait()
    d = text()
    for l in d.splitlines():
        if l.startswith("[C4] ") and not l.startswith("[C4] cpu "): print("   ", l[:200])
    if o.expect_fail:
        bad = "[C4] RESULT PASS" not in d
        report("negative control (no driver locks) detected", bad,
               "guest PASSED without locks: the test does not see races")
        finish()
    report("guest ran to completion", done and "[C4] SKIP" not in d)
    report("guest verdict", "[C4] RESULT PASS" in d)
    m = next((m for m in FAIL_MARKERS if m in d), None) or (HEAP_BAD.search(d) and "heap")
    report("debugcon clean (no lockdep / lock bug / heap / exception)", not m, str(m))
    if not done: finish()

    r = subprocess.run(["fsck.vfat", "-n", sda], capture_output=True, text=True)
    report("FAT32 fsck.vfat -n clean", r.returncode == 0, (r.stdout + r.stderr)[-400:])
    r = subprocess.run(["bash", f"{REPO}/tools/exfat-host.sh", "fsck", sdb], capture_output=True, text=True)
    report("exFAT fsck.exfat -n clean", r.returncode == 0, (r.stdout + r.stderr)[-400:])
    r = subprocess.run(["python3", "-I", f"{REPO}/tools/exfat_img.py", "deepcheck", sdb], capture_output=True, text=True)
    report("exFAT deepcheck clean", r.returncode == 0, (r.stdout + r.stderr)[-400:])
    r = subprocess.run(["python3", "-I", f"{REPO}/tools/exfat_img.py", "dirty", sdb], capture_output=True, text=True)
    report("exFAT VolumeDirty clear after umount", r.stdout.strip() == "0", r.stdout.strip())

    fdir = os.path.join(out, "fat_tree"); xdir = os.path.join(out, "exfat_tree")
    shutil.rmtree(fdir, ignore_errors=True); shutil.rmtree(xdir, ignore_errors=True)
    os.makedirs(fdir); os.makedirs(xdir)
    subprocess.run(["mcopy", "-s", "-n", "-i", sda, "::*", fdir], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    check_tree(fdir, 0, report, "FAT32")
    subprocess.run(["bash", f"{REPO}/tools/exfat-host.sh", "run", sdb, f'cp -r "$M"/. "{xdir}"/'], check=False)
    check_tree(xdir, 1, report, "exFAT")

    ref = {}
    for line in open(o.ntfs + ".list", encoding="utf-8", errors="surrogateescape"):
        t = line.rstrip("\n").split("\t", 3)
        if len(t) == 4 and t[0] in ("F", "E"): ref[t[3]] = tuple(t[:3])
    # ntfs-3g reads the EFS file raw; SecOS refuses it with EACCES by design
    # (tools/ntfs_img.py EXPECT_ERR).
    if "/secret.efs" in ref: ref["/secret.efs"] = ("E", "13", ref["/secret.efs"][1])
    ours = {}
    for line in d.splitlines():
        if line.startswith("[C4N] "):
            t = line[6:].split("\t", 3)
            if len(t) == 4: ours[t[3]] = tuple(t[:3])
    diff = [k for k in set(ref) | set(ours) if ref.get(k) != ours.get(k)]
    report(f"NTFS: {len(ours)} files read by 6 concurrent readers match ntfs-3g", not diff and len(ours) > 100,
           f"{len(diff)} differ, e.g. {[(k, ref.get(k), ours.get(k)) for k in diff[:3]]}")
    finish()

if __name__ == "__main__":
    main()
