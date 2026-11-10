#!/usr/bin/env python3
"""netposix_drive.py - the NETPOSIX gate guest driver (U10).

Runs INSIDE a private network namespace (tools/netposix_test.sh starts it
under `unshare -rmn`, brings lo up and bind-mounts a resolv.conf that points
slirp's DNS forwarder at 127.0.0.1): the host side of every test is a service
started here on 127.0.0.1, which the guest reaches as 10.0.2.2 through QEMU
user networking, and nothing can leak to or collide with the real network.

Host services: TCP echo :7001, UDP echo :7002, nothing on :7009 (TCP refused)
and :7010 (UDP: slirp turns the host's ECONNREFUSED into ICMP port
unreachable), a DNS stub on :53 (UDP + TCP; a truncated UDP answer forces
musl's TCP fallback), an NTP server on :123 whose clock runs a fixed offset
ahead, and a TCP client for the guest's own server (hostfwd).

The guest (ISO with CONSOLE=root, the test binaries on an ext2 disk at /mnt)
is driven over its serial root bash. Every check prints
"  [PASS] NETPOSIX <what>" / "  [FAIL] NETPOSIX <what>"; the last line is
"RESULT: N passed, M failed".
SPDX-License-Identifier: MIT
"""
import argparse, os, re, socket, struct, subprocess, sys, threading, time

ap = argparse.ArgumentParser()
ap.add_argument("--iso", required=True)
ap.add_argument("--disk", required=True, help="ext2 image with the nptest binaries")
ap.add_argument("--smp", default="2")
ap.add_argument("--mem", default="1G")
ap.add_argument("--nic", default="e1000")
ap.add_argument("--qemu-extra", default="")
ap.add_argument("--workdir", required=True)
ap.add_argument("--tag", default="netposix")
ap.add_argument("--udrv", action="store_true", help="expect the ring-3 e1000e driver (M58)")
ap.add_argument("--quick", action="store_true", help="skip the slow checks")
a = ap.parse_args()

W = a.workdir
LOG = os.path.join(W, a.tag + ".dbg")
SER = os.path.join(W, a.tag + ".ser")
PASSN = FAILN = 0

def ok(what, cond, detail=""):
    global PASSN, FAILN
    if cond:
        PASSN += 1; print("  [PASS] NETPOSIX %s" % what)
    else:
        FAILN += 1; print("  [FAIL] NETPOSIX %s%s" % (what, (" (" + detail + ")") if detail else ""))
    sys.stdout.flush()

NTP = {"offset": 86400 * 3 + 1234}      # the NTP server's clock: 3 days + 1234 s ahead (changed later)

# ------------------------------------------------------------------ host services
def serve_tcp(port, handler):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port)); s.listen(64)
    def loop():
        while True:
            c, _ = s.accept()
            threading.Thread(target=handler, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()

def h_echo(c):
    try:
        while True:
            d = c.recv(65536)
            if not d: break
            c.sendall(d)
    except OSError:
        pass
    c.close()

def serve_udp_echo(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", port))
    def loop():
        while True:
            d, peer = s.recvfrom(65536); s.sendto(d, peer)
    threading.Thread(target=loop, daemon=True).start()

DNS_STATS = {"udp": 0, "tcp": 0}
def dns_name(pkt, off):
    labels = []
    while True:
        n = pkt[off]; off += 1
        if n == 0: break
        labels.append(pkt[off:off + n].decode("ascii", "replace")); off += n
    return ".".join(labels).lower(), off

def dns_answer(q, tcp):
    qid, flags, qd = struct.unpack(">HHH", q[:6])
    name, off = dns_name(q, 12)
    qtype, qclass = struct.unpack(">HH", q[off:off + 4])
    question = q[12:off + 4]
    answers, rcode, tc = [], 0, 0
    def a_rr(ip):
        return b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + socket.inet_aton(ip)
    if name == "np-test.secos":
        if qtype == 1: answers = [a_rr("10.9.8.7")]
    elif name == "big.secos":
        if qtype == 1:
            if tcp: answers = [a_rr("10.77.0.%d" % i) for i in range(1, 31)]
            else: tc = 1
    elif name == "hosts-entry.secos":
        if qtype == 1: answers = [a_rr("10.66.6.6")]   # /etc/hosts must win
    elif name in ("pool.ntp.org", "ntp.secos"):
        if qtype == 1: answers = [a_rr("10.0.2.2")]
    else:
        rcode = 3                                       # NXDOMAIN
    hflags = 0x8000 | 0x0400 | (flags & 0x0100) | 0x0080 | (0x0200 if tc else 0) | rcode
    return struct.pack(">HHHHHH", qid, hflags, 1, len(answers), 0, 0) + question + b"".join(answers)

def serve_dns():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind(("127.0.0.1", 53))
    def uloop():
        while True:
            q, peer = u.recvfrom(4096)
            DNS_STATS["udp"] += 1
            try: u.sendto(dns_answer(q, False), peer)
            except Exception: pass
    threading.Thread(target=uloop, daemon=True).start()
    def th(c):
        try:
            while True:
                hdr = c.recv(2)
                if len(hdr) < 2: break
                n = struct.unpack(">H", hdr)[0]; q = b""
                while len(q) < n:
                    d = c.recv(n - len(q))
                    if not d: break
                    q += d
                DNS_STATS["tcp"] += 1
                r = dns_answer(q, True)
                c.sendall(struct.pack(">H", len(r)) + r)
        except Exception:
            pass
        c.close()
    serve_tcp(53, th)

NTP_STATS = {"n": 0}
def serve_ntp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 123))
    def ts(t):
        t += 2208988800
        return struct.pack(">II", int(t), int((t - int(t)) * 4294967296.0))
    def loop():
        while True:
            q, peer = s.recvfrom(512)
            if len(q) < 48: continue
            NTP_STATS["n"] += 1
            vn = (q[0] >> 3) & 7
            rx = time.time() + NTP["offset"]
            hdr = bytes([(0 << 6) | (vn << 3) | 4, 1, q[2], 0xEC]) + struct.pack(">II", 0, 0x10) + b"GPS\0"
            pkt = hdr + ts(rx - 1) + q[40:48] + ts(rx) + ts(time.time() + NTP["offset"])
            s.sendto(pkt, peer)
    threading.Thread(target=loop, daemon=True).start()

serve_tcp(7001, h_echo)
serve_udp_echo(7002)
serve_dns()
serve_ntp()

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
P_FWD = free_port()

# ------------------------------------------------------------------ guest
cmd = ["qemu-system-x86_64", "-cdrom", a.iso, "-boot", "d", "-m", a.mem, "-smp", a.smp,
       "-cpu", "max", "-display", "none", "-no-reboot", "-serial", "stdio",
       "-debugcon", "file:" + LOG, "-global", "isa-debugcon.iobase=0xe9",
       "-drive", "file=%s,if=virtio,format=raw" % a.disk,
       "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:8090" % P_FWD,
       "-device", a.nic + ",netdev=n0"] + a.qemu_extra.split()
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
buf = bytearray(); lk = threading.Lock(); serf = open(SER, "wb")
def rd():
    while True:
        b = p.stdout.read1(65536) if hasattr(p.stdout, "read1") else p.stdout.read(1)
        if not b: break
        with lk: buf.extend(b)
        serf.write(b); serf.flush()
threading.Thread(target=rd, daemon=True).start()

def text():
    with lk: return buf.decode("utf-8", "replace")
def send(s):
    p.stdin.write(s.encode() + b"\r"); p.stdin.flush()
def waitfor(rx, secs, start=0):
    end = time.time() + secs
    while time.time() < end:
        m = re.search(rx, text()[start:], re.M)
        if m: return m
        if p.poll() is not None: return None
        time.sleep(0.2)
    return re.search(rx, text()[start:], re.M)
SEQ = [0]
def run(command, secs=60):
    """Run a command on the serial bash; returns (rc, output)."""
    SEQ[0] += 1; tag = "Q%dQ" % SEQ[0]
    start = len(text())
    send("%s; echo %s-RC-$?" % (command, tag))
    m = waitfor(r"^%s-RC-(\d+)" % tag, secs, start)
    out = text()[start:]
    if not m: return -1, out
    return int(m.group(1)), out[:m.start()]
def sh_quote(s):
    return "'" + s.replace("'", "'\\''") + "'"

def battery(name, out):
    lines = re.findall(r"^NP (PASS|FAIL) (.*?)\r?$", out, re.M)
    for st, what in lines:
        ok("%s: %s" % (name, what), st == "PASS")
    m = re.search(r"^NP DONE \S+ pass=(\d+) fail=(\d+)", out, re.M)
    ok("%s: battery completed with no failure" % name, m is not None and m.group(2) == "0" and len(lines) > 0,
       "no DONE line" if not m else "fail=" + m.group(2))

try:
    m = waitfor(r"bash-[0-9.]+# ", 300)
    ok("boot to the root console", m is not None)
    if not m: raise SystemExit
    send("stty -echo")
    time.sleep(1)
    if a.udrv:
        ok("ring-3 e1000e driver attached (M58)", waitfor(r"\[UDRV\] eth0 driver attached", 60) is not None or
           "eth0 driver attached" in open(LOG, errors="replace").read())
    # init ran ifup (sysinit) before the console: the lease and resolv.conf
    rc, out = run("for i in $(seq 1 40); do grep -q nameserver /etc/resolv.conf 2>/dev/null && break; sleep 0.5; done; cat /etc/resolv.conf", 60)
    ok("ifup wrote /etc/resolv.conf from the DHCP lease", rc == 0 and "nameserver 10.0.2.3" in out and "# Generated by ifup" in out, out[-200:])
    rc, out = run("grep -q ' /mnt ' /proc/mounts && ls /mnt", 30)
    ok("test disk mounted at /mnt", rc == 0 and "nptest" in out, out[-200:])
    rc, out = run("grep -c 'BEGIN CERTIFICATE' /etc/ssl/certs/ca-certificates.crt; ls -l /bin/ping /bin/ntpd /bin/hwclock; head -3 /etc/services; grep -w icmp /etc/protocols; cat /etc/hosts", 30)
    ok("CA bundle installed (118 certificates)", re.search(r"^118\r?$", out, re.M) is not None, out[:200])
    ok("ping, ntpd, hwclock, /etc/services, /etc/protocols, /etc/hosts installed",
       rc == 0 and "/bin/ping" in out and "/bin/ntpd" in out and "/bin/hwclock" in out and "icmp" in out and "localhost" in out, out[-300:])
    rc, out = run("grep -E '^(ntp|rtc):' /etc/inittab", 20)
    ok("default inittab: NTP step at boot, RTC at shutdown",
       "ntp:once:/bin/ntpd -n -q -p pool.ntp.org" in out and "rtc:shutdown:/bin/hwclock -w -u" in out, out[-200:])
    run("echo '10.1.2.3 hosts-entry.secos' >> /etc/hosts", 10)
    rc, base_stats = run("/bin/netstress stats", 20)
    # an unprivileged user (and a second one for the permission test)
    rc, out = run("useradd -m npuser; useradd -m npother; id npuser; id npother", 30)
    ok("test users created", rc == 0 and "uid=" in out, out[-200:])

    rc, out = run("/mnt/nptest net 10.0.2.2", 240)
    battery("root net", out)
    rc, out = run("su -c '/mnt/nptest net 10.0.2.2' npuser", 240)
    battery("user net", out)
    ok("DNS: the stub saw UDP and TCP (truncation fallback) queries", DNS_STATS["udp"] > 0 and DNS_STATS["tcp"] > 0, str(DNS_STATS))
    rc, out = run("/mnt/nptest local /tmp", 240)
    battery("root local", out)
    rc, out = run("su -c '/mnt/nptest local /tmp' npuser", 240)
    battery("user local", out)

    # AF_UNIX path permissions: a 0700 socket file refuses another user
    run("rm -f /tmp/np-perm.sock", 10)
    send("su -c 'umask 077; /mnt/nptest userver /tmp/np-perm.sock' npuser &")
    m = waitfor(r"^NP USERVER ready", 20)
    rc, out = run("su -c '/mnt/nptest uconnect /tmp/np-perm.sock' npother", 30)
    ok("AF_UNIX: connect to another user's 0700 socket file -> EACCES", "NP UCONNECT errno=13" in out, out[-200:])
    rc, out = run("chmod 0777 /tmp/np-perm.sock; su -c '/mnt/nptest uconnect /tmp/np-perm.sock' npother; wait", 30)
    ok("AF_UNIX: after chmod 0777 the other user connects and talks", "NP UCONNECT errno=0" in out, out[-200:])

    # raw sockets
    rc, out = run("su -c '/mnt/nptest raw 10.0.2.2' npuser", 30)
    ok("SOCK_RAW refused for a normal user without CAP_NET_RAW (EPERM)", "NP RAW errno=1" in out, out[-200:])
    dbg = open(LOG, errors="replace").read()
    ok("the refusal is audited (NET_RAW)", re.search(r"\[AUDIT\].*NET_RAW", dbg) is not None)
    rc, out = run("/mnt/nptest raw 10.0.2.2", 30)
    ok("SOCK_RAW allowed for root: echo reply seen with its IP header", "NP RAW errno=0 reply=1" in out, out[-200:])
    rc, out = run("su -c '/mnt/nptest_raw raw 10.0.2.2' npuser", 30)
    ok("SOCK_RAW allowed for a normal user running a CAP_NET_RAW-signed program", "NP RAW errno=0 reply=1" in out, out[-200:])
    rc, out = run("/mnt/nptest_nonet nonet", 30)
    ok("no CAP_NET: AF_INET refused (EACCES), AF_UNIX still allowed", "NP NONET inet=-1 errno=13 unix=1" in out, out[-200:])

    # iputils ping
    rc, out = run("su -c 'ping -v -c 3 -i 0.3 10.0.2.2' npuser", 60)
    ok("ping as a normal user: ICMP ping socket (SOCK_DGRAM), 3/3 replies",
       rc == 0 and "SOCK_DGRAM" in out and re.search(r"3 packets transmitted, 3 received", out) is not None, out[-400:])
    rc, out = run("ping -v -c 2 -i 0.3 -e 0 10.0.2.2", 60)
    ok("ping as root with -e 0: raw socket (SOCK_RAW), 2/2 replies",
       rc == 0 and "SOCK_RAW" in out and re.search(r"2 packets transmitted, 2 received", out) is not None, out[-400:])
    rc, out = run("su -c 'ping -c 1 -e 0 10.0.2.2' npuser", 30)
    ok("ping -e 0 (raw) refused for a normal user", rc != 0, out[-200:])
    rc, out = run("su -c 'ping -c 6 -i 0.2 10.0.2.2 > /tmp/p1.out 2>&1 & ping -c 6 -i 0.2 10.0.2.2 > /tmp/p2.out 2>&1; wait' npuser; cat /tmp/p1.out /tmp/p2.out", 60)
    ok("two concurrent pings: 6/6 each, no duplicates or foreign replies",
       len(re.findall(r"6 packets transmitted, 6 received", out)) == 2 and "DUP" not in out and "wrong data" not in out, out[-500:])
    rc, out = run("su -c 'ping -c 2 -W 2 192.0.2.1' npuser", 60)
    ok("ping: ICMP error delivered (Destination Net Unreachable from the gateway)",
       "Destination Net Unreachable" in out and "From 10.0.2.2" in out, out[-400:])
    rc, out = run("su -c 'ping -c 2 -t 1 -i 0.3 10.0.2.2' npuser", 30)
    ok("ping -t (IP_TTL) accepted", "2 packets transmitted" in out, out[-300:])

    # time sync: init's boot-time step (ntp:once, pool.ntp.org -> the stub)
    rc, out = run("date +%s", 20)
    nums = [int(x) for x in re.findall(r"^(\d{9,})\r?$", out, re.M)]
    ok("init stepped the clock at boot (ifup -> resolv.conf -> ntpd -q -p pool.ntp.org)",
       len(nums) == 1 and abs(nums[0] - (time.time() + NTP["offset"])) < 60, "guest-host=%s" % ([n - int(time.time()) for n in nums]))
    NTP["offset"] += 5000
    rc, out = run("date +%s; ntpd -n -q -p ntp.secos; echo NTP-RC=$?; date +%s", 60)
    nums = [int(x) for x in re.findall(r"^(\d{9,})\r?$", out, re.M)]
    stepped = len(nums) == 2 and abs((nums[1] - nums[0]) - 5000) < 30
    ok("ntpd -n -q steps the clock again when the server moves 5000 s", "NTP-RC=0" in out and stepped,
       "%s n=%d" % (nums, NTP_STATS["n"]))
    rc, out = run("hwclock -w -u && hwclock -r -u", 30)
    m = re.search(r"([A-Z][a-z]{2} [A-Z][a-z]{2} +\d+ \d\d:\d\d:\d\d \d{4})", out)
    rtc = None
    if m:
        import calendar
        try: rtc = calendar.timegm(time.strptime(" ".join(m.group(1).split()), "%a %b %d %H:%M:%S %Y"))
        except ValueError: rtc = None
    ok("hwclock -w then -r through /dev/rtc0", rc == 0 and rtc is not None, out[-200:])
    ok("the RTC now holds the NTP time", rtc is not None and abs(rtc - (time.time() + NTP["offset"])) < 120,
       "rtc-host=%s" % (None if rtc is None else int(rtc - time.time())))

    # the guest as a TCP server (host client through hostfwd)
    send("/mnt/nptest server 8090 3 &")
    waitfor(r"^NP SERVER ready", 20)
    good = 0
    for i in range(3):
        try:
            c = socket.create_connection(("127.0.0.1", P_FWD), timeout=10)
            msg = os.urandom(20000)
            c.sendall(msg); c.shutdown(socket.SHUT_WR)
            got = b""
            while True:
                d = c.recv(65536)
                if not d: break
                got += d
            c.close()
            good += got == msg
        except OSError as e:
            print("    host client: %s" % e)
    m = waitfor(r"^NP SERVER served=(\d+)", 30)
    ok("guest TCP server (accept4): 3 host connections echoed", good == 3 and m is not None and m.group(1) == "3", "good=%d" % good)

    # leaks: the socket table is back to its baseline
    time.sleep(3)
    rc, stats = run("/bin/netstress stats", 20)
    def sockets(s):
        m = re.search(r"sockets=(\d+)", s); return int(m.group(1)) if m else -1
    ok("no socket leak (socket table back to its baseline)", sockets(stats) >= 0 and sockets(stats) <= sockets(base_stats),
       "before=%d after=%d" % (sockets(base_stats), sockets(stats)))

    send("poweroff")
    for _ in range(90):
        if p.poll() is not None: break
        time.sleep(1)
    ok("poweroff (runs the rtc shutdown entry)", p.poll() is not None)
finally:
    if p.poll() is None:
        p.kill()
    p.wait()
    dbg = open(LOG, errors="replace").read() if os.path.exists(LOG) else ""
    ok("no kernel exception, panic, SMAP or lockdep report",
       re.search(r"\[PANIC\]|KERNEL PANIC|\[LOCKDEP\]|\[SMAP\] violation|\[EXC\]|\[TLB\] BUG", dbg) is None)
    print("RESULT: %d passed, %d failed" % (PASSN, FAILN))
sys.exit(1 if FAILN else 0)
