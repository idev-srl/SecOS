#!/usr/bin/env python3
"""net_stress_drive.py - host side of the M47 network stack stress gate.

Boots a SecOS image (built with a baked SSH key, see --key) under QEMU user
networking and hammers the network stack from several guest processes and
several host connections at once:

  * host peers on 127.0.0.1 (the guest reaches them as 10.0.2.2): a TCP echo
    server, a TCP firehose (streams until the guest closes), a UDP sink and a
    tiny DNS server answering A queries;
  * /bin/netstress client (guest, forked workers spread over the CPUs): TCP
    echo round trips, firehose connections closed while data is arriving, UDP
    floods, DNS lookups;
  * /bin/netstress server (guest) accepting parallel host connections, some of
    which the host resets in the middle of a transfer;
  * extra ssh sessions (Dropbear) running in parallel with all of it.

The stack occupancy (socket table, TCP control blocks, UDP bindings) and
MemFree are sampled with `netstress stats` before and after each stress round
to catch leaks. Results are written as "[NSD] <name> OK|FAIL <detail>" lines.
"""
import argparse
import random
import socket
import struct
import subprocess
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument("--iso", required=True)
ap.add_argument("--smp", default="1")
ap.add_argument("--key", required=True, help="private key whose .pub is baked into the image")
ap.add_argument("--log", required=True, help="debugcon log")
ap.add_argument("--out", required=True, help="driver results")
ap.add_argument("--nproc", type=int, default=4)
ap.add_argument("--rounds", type=int, default=4)
ap.add_argument("--conns", type=int, default=12)
ap.add_argument("--stress-rounds", type=int, default=2)
ap.add_argument("--nic", default="e1000", help="QEMU NIC model (e1000e exercises MSI-X NAPI RX)")
args = ap.parse_args()

out = open(args.out, "w")
results = []
lock = threading.Lock()


def log(s):
    with lock:
        out.write(s + "\n")
        out.flush()


def check(name, ok, detail=""):
    results.append(ok)
    log("[NSD] %s %s %s" % (name, "OK" if ok else "FAIL", detail))


def free_port():
    while True:
        p = random.randint(20000, 60000)
        if p in (2222, 8000):
            continue
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", p))
            s.close()
            return p
        except OSError:
            s.close()


# ---------------------------------------------------------------- host peers
counters = {"echo": 0, "fire": 0, "udp": 0, "dns": 0}


def serve_tcp(port, handler):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(64)

    def loop():
        while True:
            c, _ = ls.accept()
            threading.Thread(target=handler, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()


def h_echo(c):
    with lock:
        counters["echo"] += 1
    try:
        c.settimeout(60)
        while True:
            d = c.recv(65536)
            if not d:
                break
            c.sendall(d)
    except OSError:
        pass
    c.close()


def h_fire(c):
    with lock:
        counters["fire"] += 1
    blk = bytes(range(256)) * 16
    try:
        c.settimeout(30)
        while True:
            c.sendall(blk)
    except OSError:
        pass
    c.close()


def serve_udp_sink(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))

    def loop():
        while True:
            s.recvfrom(4096)
            with lock:
                counters["udp"] += 1
    threading.Thread(target=loop, daemon=True).start()


def serve_dns(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))

    def loop():
        while True:
            q, a = s.recvfrom(512)
            if len(q) < 17:
                continue
            o, labels = 12, []
            while o < len(q) and q[o]:
                labels.append(q[o + 1:o + 1 + q[o]].decode(errors="replace"))
                o += 1 + q[o]
            qend = o + 5
            ip = (10, 0, 0, 0)
            try:
                w = labels[0]
                wid = int(w[1:w.index("r")])
                rnd = int(w[w.index("r") + 1:w.index("q")])
                qq = int(w[w.index("q") + 1:])
                ip = (10, wid, rnd, qq)
            except (ValueError, IndexError):
                pass
            hdr = q[:2] + b"\x81\x80" + q[4:6] + b"\x00\x01\x00\x00\x00\x00"
            ans = b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04" + bytes(ip)
            s.sendto(hdr + q[12:qend] + ans, a)
            with lock:
                counters["dns"] += 1
    threading.Thread(target=loop, daemon=True).start()


P_ECHO, P_FIRE, P_UDP, P_DNS = free_port(), free_port(), free_port(), free_port()
P_SSH, P_LISTEN = free_port(), free_port()
serve_tcp(P_ECHO, h_echo)
serve_tcp(P_FIRE, h_fire)
serve_udp_sink(P_UDP)
serve_dns(P_DNS)
GUEST_LISTEN = 7777

# ---------------------------------------------------------------- guest
qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", args.iso, "-boot", "d", "-m", "512M", "-smp", args.smp,
     "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:22,hostfwd=tcp:127.0.0.1:%d-:%d"
     % (P_SSH, P_LISTEN, GUEST_LISTEN),
     "-device", args.nic + ",netdev=n0",
     "-debugcon", "file:" + args.log, "-global", "isa-debugcon.iobase=0xe9",
     "-no-reboot", "-display", "none", "-serial", "null"],
    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def ssh(cmd, timeout=120):
    try:
        r = subprocess.run(
            ["ssh", "-p", str(P_SSH), "-i", args.key, "-o", "IdentitiesOnly=yes",
             "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
             "-o", "BatchMode=yes", "-o", "ConnectTimeout=30", "-o", "LogLevel=ERROR",
             "root@127.0.0.1", cmd],
            capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"


def stats(tag):
    rc, o, e = ssh("/bin/netstress stats")
    log("[NSD] stats %s rc=%d %s" % (tag, rc, o.strip()))
    d = {}
    for tok in o.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                d[k] = int(v)
            except ValueError:
                pass
    return d


def host_conn(i, res):
    """Connect to the guest's netstress server. Every third connection sends
    8 KB and then resets (data in flight on the guest side)."""
    data = bytes((i * 7 + k) & 0xff for k in range(32768))
    try:
        s = socket.create_connection(("127.0.0.1", P_LISTEN), timeout=30)
        if i % 3 == 2:
            s.sendall(data[:8192])
            time.sleep(0.05)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            s.close()
            res.append(("reset", True))
            return
        got = bytearray()

        def rx():
            while len(got) < len(data):
                b = s.recv(65536)
                if not b:
                    break
                got.extend(b)
        t = threading.Thread(target=rx)
        t.start()
        s.sendall(data)
        t.join(60)
        s.shutdown(socket.SHUT_WR)
        s.close()
        res.append(("echo", bytes(got) == data))
    except OSError as ex:
        res.append(("echo", False))
        log("[NSD] host conn %d error %s" % (i, ex))


try:
    up = False
    t0 = time.time()
    while time.time() - t0 < 150:
        rc, o, _ = ssh("echo NSD-UP; nproc", timeout=40)
        if rc == 0 and "NSD-UP" in o:
            up = True
            break
        time.sleep(3)
    check("ssh-up", up, "after %ds cpus=%s" % (time.time() - t0, o.split()[-1] if up else "?"))
    if not up:
        raise SystemExit

    rc, o, e = ssh("/bin/netstress client %d %d %d %d 2 1" % (P_ECHO, P_FIRE, P_UDP, P_DNS))
    check("warmup-client", rc == 0 and "DONE pass=2 fail=0" in o, o.strip().replace("\n", " | "))
    time.sleep(13)                     # orphans (10 s) + TIME-WAIT (2 s) settle
    base = stats("base")

    first = base
    for rnd in range(1, args.stress_rounds + 1):
        res = {}
        udp0 = counters["udp"]

        def run(name, cmd):
            res[name] = ssh(cmd, timeout=300)
        ths = [threading.Thread(target=run, args=("server", "/bin/netstress server %d %d"
                                                  % (GUEST_LISTEN, args.conns)))]
        ths[0].start()
        time.sleep(2)                  # let the listener come up
        ths.append(threading.Thread(target=run, args=("client", "/bin/netstress client %d %d %d %d %d %d"
                                    % (P_ECHO, P_FIRE, P_UDP, P_DNS, args.nproc, args.rounds))))
        ths.append(threading.Thread(target=run, args=("ssh1", "seq 1 3000 > /tmp/nsd1; wc -l < /tmp/nsd1; echo SSH1-OK")))
        ths.append(threading.Thread(target=run, args=("ssh2", "cat /proc/meminfo > /tmp/nsd2; echo SSH2-OK")))
        for t in ths[1:]:
            t.start()
        hres = []
        hth = [threading.Thread(target=host_conn, args=(i, hres)) for i in range(args.conns)]
        for i, t in enumerate(hth):
            t.start()
            if i % 4 == 3:
                time.sleep(0.3)
        for t in hth:
            t.join(120)
        for t in ths:
            t.join(320)
        rc, o, e = res.get("client", (-1, "", "missing"))
        check("r%d-client" % rnd, rc == 0 and ("DONE pass=%d fail=0" % args.nproc) in o,
              o.strip().replace("\n", " | ") + " " + e.strip())
        rc, o, e = res.get("server", (-1, "", "missing"))
        # A connection the host resets before accept() is dropped by the kernel
        # (never queued), so only the echo connections are guaranteed.
        nacc = 0
        for tok in o.split():
            if tok.startswith("conns="):
                nacc = int(tok[6:])
        neach = sum(1 for i in range(args.conns) if i % 3 != 2)
        check("r%d-server" % rnd, rc == 0 and "server done" in o and neach <= nacc <= args.conns,
              o.strip().replace("\n", " | ") + " " + e.strip())
        echo_ok = [ok for kind, ok in hres if kind == "echo"]
        check("r%d-host-echo" % rnd, len(echo_ok) > 0 and all(echo_ok),
              "%d/%d echo conns intact, %d resets" % (sum(echo_ok), len(echo_ok),
                                                     sum(1 for k, _ in hres if k == "reset")))
        rc, o, e = res.get("ssh1", (-1, "", ""))
        check("r%d-ssh-parallel-1" % rnd, rc == 0 and "3000" in o and "SSH1-OK" in o,
              "rc=%d %s %s" % (rc, o.strip().replace("\n", " "), e.strip()))
        rc, o, e = res.get("ssh2", (-1, "", ""))
        check("r%d-ssh-parallel-2" % rnd, rc == 0 and "SSH2-OK" in o,
              "rc=%d %s %s" % (rc, o.strip().replace("\n", " "), e.strip()))
        sent = args.nproc * args.rounds * 100
        got = counters["udp"] - udp0
        check("r%d-udp-flood" % rnd, got >= sent // 2, "%d/%d datagrams reached the host" % (got, sent))
        time.sleep(13)
        cur = stats("r%d" % rnd)
        for k in ("sockets", "tcp", "orphans", "udp"):
            check("r%d-no-leak-%s" % (rnd, k), k in cur and cur.get(k) == base.get(k),
                  "base=%s now=%s" % (base.get(k), cur.get(k)))
        if rnd == 1:
            first = cur
        else:
            # The kernel heap keeps what it grew to (high-water mark), so allow
            # some slack; a per-connection leak (128 KB of TCP buffers per
            # connection, ~100 connections per round) would blow far past it.
            drop = first.get("memfree", 0) - cur.get("memfree", 0)
            check("r%d-memfree-stable" % rnd, "memfree" in cur and drop <= 2048,
                  "after-r1=%s now=%s kB (drop %d kB)" % (first.get("memfree"), cur.get("memfree"), drop))
    rc, o, _ = ssh("echo NSD-ALIVE")
    check("alive-after", rc == 0 and "NSD-ALIVE" in o, "")
    log("[NSD] host counters %s" % counters)
    rc, o, _ = ssh("tail -n 15 /var/log/sshd.log")
    for line in o.splitlines():
        log("[NSD] sshd.log: " + line)
finally:
    qemu.terminate()
    try:
        qemu.wait(10)
    except subprocess.TimeoutExpired:
        qemu.kill()
    log("[NSD] RESULT %s pass=%d fail=%d" % ("PASS" if results and all(results) else "FAIL",
                                            sum(results), len(results) - sum(results)))
    out.close()
