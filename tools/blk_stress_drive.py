#!/usr/bin/env python3
# tools/blk_stress_drive.py — block-layer + USB lock stress (M50 B3).
# SPDX-License-Identifier: MIT
#
# Boots an ISO built with CONSOLE=root and -DSECOS_B3_BLKSTRESS on q35 with
# four disks behind three different drivers plus a USB keyboard on the same
# xHCI controller as a USB stick:
#   sda      AHCI port 0, 32 MiB of seeded random data
#   sdb      AHCI port 1, an MBR with one partition (sdb1) of random data
#   nvme0n1  NVMe, 32 MiB of random data
#   usb0     usb-storage on qemu-xhci, 16 MiB of random data (+ usb-kbd)
#
# Phase 1 (kernel, every CPU at once, outside the BKL): fs/blkstress.c hashes
# regions of all four disks for several rounds, writes a pattern to nvme0n1
# and two interleaved byte-stripe patterns to sdb1 through devfs, while every
# CPU polls the HID keyboard. As soon as it starts, this script types a line
# on the usb-kbd through QMP. Checked: the kernel's digests against the image
# files, its own write verification, and the typed line.
# Phase 2 (userland, root bash on the serial console): parallel dd reads of
# /dev/sda, /dev/nvme0n1, /dev/sdb1 and /dev/usb0 piped to md5sum, and two
# parallel unaligned dd copies disk-to-disk (bs=1000 and bs=700, so devfs does
# partial-sector read-modify-write), while another line is typed on the
# usb-kbd into a foreground `read`. Checked: the md5s against the images, the
# typed line, and after QEMU quits the bytes the guest wrote (phase 1 patterns
# and phase 2 copies) in the image files.
#
# Output: "[BSD] <step> OK|FAIL ..." lines and "[BSD] RESULT ok=N fail=M";
# exit 0 when every step passed.
#
# Usage: blk_stress_drive.py --iso ISO [--smp 4] [--out DIR] [--log LOG]
import argparse, hashlib, json, os, random, re, socket, subprocess, sys, tempfile, threading, time

MIB = 1024 * 1024
SEC = 512
PART_START = 2048                     # sdb1 first sector
FAIL_MARKERS = ["[EXC] int=", "PANIC", "nested fault", "stack smashing",
                "[LOCKDEP]", "[LOCK] BUG", "shootdown timeout", "CORRUPTION"]
# Heap corruption reports; the boot heap self-test's "(expected)" lines are fine.
HEAP_BAD = re.compile(r"\[HEAP\] (?!\(expected\))[^\n]*HEAP:")
KEYS1 = "bthreekeysphaseone0123456789abcdefghijklmnopqrstuvwxyz"
KEYS2 = "userlandkeys9876543210zyxwvutsrqponm"

def mix64(z):
    M = (1 << 64) - 1
    z = (z + 0x9E3779B97F4A7C15) & M
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M
    return z ^ (z >> 31)

def pattern(tag, x, n):
    # fs/blkstress.c pat_fill: byte a = byte (a & 7) of mix64((tag << 48) ^ (a >> 3)).
    w0, w1 = x >> 3, (x + n + 7) >> 3
    raw = b"".join(mix64((tag << 48) ^ w).to_bytes(8, "little") for w in range(w0, w1))
    return raw[x - (w0 << 3):x - (w0 << 3) + n]

def mkdisk(path, size, seed, mbr=False):
    r = random.Random(seed)
    data = bytearray(r.randbytes(size))
    data[510] = 0; data[511] = 0           # never look like an MBR / boot sector
    data[1080] = 0                         # nor like ext2 (magic 0xEF53 at 1080)
    if mbr:
        for i in range(446, 512): data[i] = 0
        e = 446
        data[e + 4] = 0x83
        cnt = size // SEC - PART_START
        data[e + 8:e + 12] = PART_START.to_bytes(4, "little")
        data[e + 12:e + 16] = cnt.to_bytes(4, "little")
        data[510] = 0x55; data[511] = 0xAA
        p0 = PART_START * SEC
        data[p0 + 510] = 0; data[p0 + 511] = 0; data[p0 + 1080] = 0
    with open(path, "wb") as f: f.write(data)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--mem", default="1G")
    ap.add_argument("--out", default=None)
    ap.add_argument("--log", default=None)
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--boot", type=float, default=300)
    o = ap.parse_args()
    out = o.out or tempfile.mkdtemp(prefix="secos_bsd.")
    os.makedirs(out, exist_ok=True)
    log = o.log or os.path.join(out, "debugcon.log")
    img = {n: os.path.join(out, n + ".img") for n in ("sda", "sdb", "nvme", "usb")}
    mkdisk(img["sda"], 32 * MIB, 101)
    mkdisk(img["sdb"], 32 * MIB, 202, mbr=True)
    mkdisk(img["nvme"], 32 * MIB, 303)
    mkdisk(img["usb"], 16 * MIB, 404)
    orig = {n: open(p, "rb").read() for n, p in img.items()}
    qmp_path = os.path.join(out, "qmp.sock")
    try: os.unlink(qmp_path)
    except OSError: pass

    qemu = [o.qemu, "-machine", "q35", "-m", o.mem, "-smp", str(o.smp),
            "-cdrom", o.iso, "-boot", "d",
            "-drive", f"file={img['sda']},if=ide,index=0,format=raw",
            "-drive", f"file={img['sdb']},if=ide,index=1,format=raw",
            "-drive", f"file={img['nvme']},if=none,id=nvm,format=raw",
            "-device", "nvme,serial=b3stress,drive=nvm",
            "-device", "qemu-xhci,id=xhci",
            "-device", "usb-kbd,bus=xhci.0,id=kbd0",
            "-drive", f"file={img['usb']},if=none,id=us,format=raw",
            "-device", "usb-storage,bus=xhci.0,drive=us",
            "-debugcon", "file:" + log, "-global", "isa-debugcon.iobase=0xe9",
            "-qmp", f"unix:{qmp_path},server=on,wait=off",
            "-no-reboot", "-display", "none", "-serial", "stdio"]
    open(log, "w").close()
    p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    buf = bytearray(); lock = threading.Lock(); alive = [True]
    def rd():
        while True:
            b = p.stdout.read1(65536)
            if not b: alive[0] = False; break
            with lock: buf.extend(b)
    threading.Thread(target=rd, daemon=True).start()

    ok = fail = 0
    def report(name, good, why=""):
        nonlocal ok, fail
        if good: ok += 1
        else: fail += 1
        print(f"[BSD] {name} {'OK' if good else 'FAIL'} {why}".rstrip(), flush=True)
    def logtext():
        try: return open(log, "rb").read().decode("latin1")
        except OSError: return ""
    def fault():
        d = logtext()
        for m in FAIL_MARKERS:
            if m in d: return m
        h = HEAP_BAD.search(d)
        return h.group(0)[:120] if h else None
    def wait_log(needle, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in logtext(): return True
            if not alive[0]: return False
            time.sleep(0.1)
        return False
    def wait_serial(needle, timeout, since=0):
        nb = needle.encode(); t0 = time.time()
        while time.time() - t0 < timeout:
            with lock:
                if nb in buf[since:]: return True
            if not alive[0]: return False
            time.sleep(0.05)
        return False
    def finish():
        try: p.kill()
        except OSError: pass
        print(f"[BSD] RESULT ok={ok} fail={fail} (files in {out})", flush=True)
        sys.exit(0 if fail == 0 else 1)

    # ---- QMP (usb-kbd typing) ----
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
            m = json.loads(line)
            if "event" in m: continue
            return m
    qmp_read()
    def qmp(cmd, **a):
        qf.write((json.dumps({"execute": cmd, "arguments": a}) + "\n").encode()); qf.flush()
        return qmp_read()
    qmp("qmp_capabilities")
    def key(qcode):
        for down in (True, False):
            # No `device`: QEMU 6.2 only accepts a display there. The guest
            # set the usb-kbd up last, so it is the active keyboard; the
            # kernel's usb_hid_keydowns count proves the keys came over USB.
            qmp("input-send-event",
                events=[{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}])
            time.sleep(0.03)
    def type_usb(s):
        for ch in s: key(ch)
        key("ret")

    # ---- phase 1: kernel stress ----
    started = wait_log("[B3] stress start", o.boot)
    report("kernel-stress-start", started)
    if not started:
        if "[B3] SKIP" in logtext(): report("kernel-stress-devices", False, "SKIP (disk missing)")
        finish()
    time.sleep(0.5)
    type_usb(KEYS1)
    done = wait_log("[B3] RESULT", 240)
    report("kernel-stress-finished", done)
    d = logtext()
    report("kernel-stress-pass", "[B3] RESULT PASS" in d,
           "; ".join(re.findall(r"\[B3\] FAIL [^\n]*", d))[:300])
    k = re.search(r"\[B3\] keys=\[([^\]\n]*)\] usb_hid_keydowns=(\d+)", d)
    report("kernel-stress-keys", bool(k) and k.group(1) == KEYS1, f"got {k.group(1)!r}" if k else "no keys line")
    # +1: Enter. Every key must have come through the USB HID driver.
    report("kernel-stress-keys-over-usb", bool(k) and int(k.group(2)) >= len(KEYS1) + 1,
           f"usb_hid_keydowns={k.group(2) if k else '?'}")
    cpus = re.search(r"\[B3\] workers=(\d+)", d)
    report("kernel-stress-all-cpus", bool(cpus) and int(cpus.group(1)) == o.smp,
           f"workers={cpus.group(1) if cpus else '?'}")
    regions = {"sda": (orig["sda"], 0), "sdb1": (orig["sdb"], PART_START * SEC),
               "nvme0n1": (orig["nvme"], 0), "usb0": (orig["usb"], 0)}
    hashes = re.findall(r"\[B3\] hash (\S+) (\d+) (\d+) ([0-9a-f]{64}) rounds=(\d+)", d)
    report("kernel-stress-hash-lines", len(hashes) == 5, f"n={len(hashes)}")
    for dev, lba, nsec, hx, rounds in hashes:
        data, base = regions[dev]
        a = base + int(lba) * SEC
        want = hashlib.sha256(data[a:a + int(nsec) * SEC]).hexdigest()
        report(f"kernel-hash-{dev}-{lba}", hx == want, f"rounds={rounds}")
    for what in ("nvme-write", "sdb1-stripes"):
        report(f"kernel-{what}-verify", f"[B3] {what} verify OK" in d)
    report("kernel-no-fault", fault() is None, str(fault() or ""))

    # ---- phase 2: userland dd in parallel + usb-kbd typing ----
    t0 = time.time(); prompt = False
    while time.time() - t0 < o.boot and alive[0]:
        with lock: tail = bytes(buf[-64:])
        if tail.rstrip().endswith(b"#"): prompt = True; break
        time.sleep(0.2)
    report("boot-prompt", prompt)
    if not prompt: finish()
    time.sleep(1.0)
    NV_SEEK, NV_CNT = 25166, 2000           # bs=1000: 25 166 000 .. 27 166 000
    SB_SEEK, SB_CNT = 17977, 1500           # bs=700 into sdb1: 12 583 900 .. 13 633 900
    cmd = ("rm -f /tmp/h?; "
           "(dd if=/dev/sda bs=65536 count=256 2>/dev/null | md5sum > /tmp/h1) & "
           "(dd if=/dev/nvme0n1 bs=4096 count=4096 2>/dev/null | md5sum > /tmp/h2) & "
           "(dd if=/dev/sdb1 bs=512 count=16384 2>/dev/null | md5sum > /tmp/h3) & "
           "(dd if=/dev/usb0 bs=65536 count=128 2>/dev/null | md5sum > /tmp/h4) & "
           f"(dd if=/dev/sda of=/dev/nvme0n1 bs=1000 count={NV_CNT} seek={NV_SEEK} 2>/dev/null; echo w1 > /tmp/h5) & "
           f"(dd if=/dev/usb0 of=/dev/sdb1 bs=700 count={SB_CNT} seek={SB_SEEK} 2>/dev/null; echo w2 > /tmp/h6) & "
           "read -r K; echo \"KEYS=[$K]\"; wait; "
           "echo \"MD5S $(cut -d' ' -f1 /tmp/h1) $(cut -d' ' -f1 /tmp/h2) $(cut -d' ' -f1 /tmp/h3) "
           "$(cut -d' ' -f1 /tmp/h4) $(cat /tmp/h5) $(cat /tmp/h6) END$((6*7))\"")
    with lock: since = len(buf)
    p.stdin.write(b"\x15" + cmd.encode() + b"\r"); p.stdin.flush()
    time.sleep(1.0)
    type_usb(KEYS2)
    got = wait_serial("END42", 600, since)
    with lock: so = bytes(buf[since:]).decode("latin1")
    report("userland-finished", got)
    k2 = re.search(r"KEYS=\[([^\]\r\n$]*)\]", so)          # not the echoed "$K"
    report("userland-keys", bool(k2) and k2.group(1) == KEYS2, f"got {k2.group(1)!r}" if k2 else "no KEYS line")
    m = re.search(r"MD5S (\S+) (\S+) (\S+) (\S+) (\S+) (\S+) END42", so)
    if m:
        want = [hashlib.md5(orig["sda"][:256 * 65536]).hexdigest(),
                hashlib.md5(orig["nvme"][:4096 * 4096]).hexdigest(),
                hashlib.md5(orig["sdb"][PART_START * SEC:PART_START * SEC + 16384 * 512]).hexdigest(),
                hashlib.md5(orig["usb"][:128 * 65536]).hexdigest()]
        for i, n in enumerate(("sda", "nvme0n1", "sdb1", "usb0")):
            report(f"userland-md5-{n}", m.group(i + 1) == want[i])
        report("userland-writes-ran", m.group(5) == "w1" and m.group(6) == "w2")
    else:
        report("userland-md5-line", False)
    report("userland-no-fault", fault() is None, str(fault() or ""))
    # Informational: the kernel lock's contention (acquired, contended, max
    # wait in us) after the storm, from /proc/lockstat.
    with lock: since = len(buf)
    p.stdin.write(b"\x15grep -E '^klock' /proc/lockstat; echo LS-$((6*7))\r"); p.stdin.flush()
    if wait_serial("LS-42", 30, since):
        with lock: so = bytes(buf[since:]).decode("latin1")
        for ln in re.findall(r"^klock +\S+ +\d+ +\d+ +\d+ +\d+", so, re.M):
            print("[BSD] lockstat " + " ".join(ln.split()), flush=True)
    qmp("quit")
    try: p.wait(timeout=20)
    except subprocess.TimeoutExpired: p.kill()

    # ---- what the guest wrote, read back from the image files ----
    nv = open(img["nvme"], "rb").read(); sb = open(img["sdb"], "rb").read()
    report("image-nvme-phase1-pattern", nv[16 * MIB:24 * MIB] == pattern(2, 16 * MIB, 8 * MIB))
    ps = PART_START * SEC
    report("image-sdb1-phase1-stripes", sb[ps + 8 * MIB:ps + 8 * MIB + 256 * 1024] ==
           pattern(3, 8 * MIB, 256 * 1024))
    a = NV_SEEK * 1000
    report("image-nvme-userland-copy", nv[a:a + NV_CNT * 1000] == orig["sda"][:NV_CNT * 1000])
    report("image-nvme-around-copy-untouched", nv[24 * MIB:a] == orig["nvme"][24 * MIB:a] and
           nv[a + NV_CNT * 1000:] == orig["nvme"][a + NV_CNT * 1000:])
    b = ps + SB_SEEK * 700
    report("image-sdb1-userland-copy", sb[b:b + SB_CNT * 700] == orig["usb"][:SB_CNT * 700])
    report("image-sdb-around-copy-untouched", sb[ps + 8 * MIB + 256 * 1024:b] == orig["sdb"][ps + 8 * MIB + 256 * 1024:b]
           and sb[b + SB_CNT * 700:] == orig["sdb"][b + SB_CNT * 700:])
    report("image-sda-usb-unchanged", open(img["sda"], "rb").read() == orig["sda"] and
           open(img["usb"], "rb").read() == orig["usb"])
    finish()

if __name__ == "__main__":
    main()
