#!/usr/bin/env python3
"""tools/net_udrv_check.py - one guest of the M58 gate (tools/net_udrv_gate.sh).

Boots a SecOS image (CONSOLE=root: bash on the serial console; a throwaway
SSH key baked in) under QEMU user networking with an e1000e NIC and checks
the network through it. Host peers listen on 127.0.0.1 (the guest reaches
them as 10.0.2.2): an HTTP file server (random payload, MD5 known), a TCP
sink that reports the MD5 of what it received, a TCP echo server. Ports are
picked by the kernel (never 2222/8000); ssh goes through a hostfwd port.

  --mode udrv      the NIC belongs to /sbin/udrv-e1000e (udrv=): the proxy
                   is attached, DHCP bound, TCP download + upload with MD5,
                   DNS through QEMU's resolver, an SSH session; with
                   --kills N: N x kill -9 of the driver while a download
                   loop runs, each time "[UDRV] eth0 driver died", a new
                   driver attaches within RESPAWN_S, traffic resumes; after
                   the loop MemFree+Slab / SUnreclaim within slack and the
                   ring-3 page counters equal the first sample;
  --mode kernel    the same image (or one) without udrv=: the in-kernel
                   e1000e drives the NIC, no [UDRV] line, same data checks.
--perf: a timed 32 MiB download, a timed upload and 200 TCP connect+echo
round trips, printed as "[nud] NAME: perf ..." (not a pass/fail check).
Always: no CPU exception, panic, lockdep report, SMAP violation.
Prints "[nud] NAME: <check> ok|FAIL ..." and "[nud] NAME: PASS|FAIL".
SPDX-License-Identifier: MIT
"""
import argparse, hashlib, http.server, json, os, re, socket, socketserver, subprocess, sys, threading, time

ap = argparse.ArgumentParser()
ap.add_argument('--name', required=True)
ap.add_argument('--workdir', required=True)
ap.add_argument('--qemu', default='qemu-system-x86_64')
ap.add_argument('--mode', required=True, choices=['udrv', 'kernel'])
ap.add_argument('--key', required=True)
ap.add_argument('--kills', type=int, default=0)
ap.add_argument('--perf', action='store_true')
ap.add_argument('--timeout', type=float, default=300)
ap.add_argument('--expect', action='append', default=[])
ap.add_argument('qemu_args', nargs=argparse.REMAINDER)
a = ap.parse_args()
qargs = a.qemu_args[1:] if a.qemu_args[:1] == ['--'] else a.qemu_args

KERNEL_BAD = [r'\[EXC\] int=', r'\[PANIC\]', r'KERNEL PANIC', r'nested fault', r'stack smashing',
              r'LOCKDEP\]', r'\[LOCK\] BUG', r'\[TLB\] BUG', r'shootdown timeout',
              r'invalidation wait TIMEOUT', r'\[SMAP\] violation', r'\[DMA\] BUG']
FRAME_SLACK_KB = 1024
HEAP_SLACK_KB = 2048
RESPAWN_S = 5.0

W = a.workdir
os.makedirs(W, exist_ok=True)
log, ser = os.path.join(W, a.name + '.log'), os.path.join(W, a.name + '.ser')
sock, pidf = os.path.join(W, a.name + '.qmp'), os.path.join(W, a.name + '.pid')
for f in (log, ser, sock, pidf):
    try: os.unlink(f)
    except FileNotFoundError: pass

fails = []
def say(s): print('[nud] %s: %s' % (a.name, s), flush=True)
def check(name, ok, detail=''):
    say('%s %s %s' % (name, 'ok' if ok else 'FAIL', detail))
    if not ok: fails.append(name)

# ------------------------------------------------------------ host peers
PAYLOAD = os.urandom(8 << 20)
BIG = os.urandom(32 << 20) if a.perf else b''
FILES = {'/f8': PAYLOAD, '/f32': BIG, '/f64k': PAYLOAD[:65536]}
MD5 = {k: hashlib.md5(v).hexdigest() for k, v in FILES.items()}

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'
    def do_GET(self):
        d = FILES.get(self.path)
        if d is None or not d:
            self.send_response(404); self.end_headers(); return
        self.send_response(200)
        self.send_header('Content-Length', str(len(d)))
        self.end_headers()
        try: self.wfile.write(d)
        except OSError: pass
    def log_message(self, *x): pass

class TS(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
httpd = TS(('127.0.0.1', 0), H)
P_HTTP = httpd.server_address[1]
threading.Thread(target=httpd.serve_forever, daemon=True).start()

sink_md5 = []
def serve_tcp(handler):
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(('127.0.0.1', 0)); ls.listen(64)
    def loop():
        while True:
            c, _ = ls.accept()
            threading.Thread(target=handler, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()
    return ls.getsockname()[1]

def h_sink(c):
    h = hashlib.md5(); n = 0
    try:
        c.settimeout(120)
        while True:
            d = c.recv(65536)
            if not d: break
            h.update(d); n += len(d)
    except OSError: pass
    sink_md5.append((n, h.hexdigest())); c.close()

def h_echo(c):
    """One request, one reply, then close (so the guest's nc exits)."""
    try:
        c.settimeout(30)
        d = c.recv(4096)
        if d: c.sendall(d)
    except OSError: pass
    c.close()

P_SINK, P_ECHO = serve_tcp(h_sink), serve_tcp(h_echo)

def free_port():
    while True:
        s = socket.socket(); s.bind(('127.0.0.1', 0)); p = s.getsockname()[1]; s.close()
        if p not in (2222, 8000): return p
P_SSH = free_port()

# ------------------------------------------------------------ guest
cmd = [a.qemu, '-display', 'none', '-no-reboot', '-serial', 'stdio',
       '-debugcon', 'file:' + log, '-global', 'isa-debugcon.iobase=0xe9',
       '-qmp', 'unix:%s,server,nowait' % sock, '-pidfile', pidf,
       '-netdev', 'user,id=n0,hostfwd=tcp:127.0.0.1:%d-:22' % P_SSH,
       '-device', 'e1000e,netdev=n0'] + qargs
err = open(os.path.join(W, a.name + '.err'), 'w')
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err)
buf = bytearray(); lk = threading.Lock(); serf = open(ser, 'wb')
def rd():
    while True:
        b = p.stdout.read1(65536)
        if not b: break
        with lk: buf.extend(b)
        serf.write(b); serf.flush()
threading.Thread(target=rd, daemon=True).start()

def dlog():
    try: return open(log, 'rb').read().decode('latin1')
    except OSError: return ''
def mark():
    with lk: return len(buf)
def out(since):
    with lk: return bytes(buf[since:]).decode('latin1')

def wait_prompt(tmo):
    t0 = time.time(); poke = 0.0
    while time.time() - t0 < tmo and p.poll() is None:
        with lk: tail = bytes(buf[-64:]); seen = b'bash-' in buf
        if tail.rstrip().endswith(b'#'): return True
        if seen and time.time() - poke > 5:
            poke = time.time(); p.stdin.write(b'\r'); p.stdin.flush()
        time.sleep(0.2)
    return False

seq = [0]
def run(c, tmo=120):
    """Run c in the serial bash; its output (CR stripped) or None."""
    seq[0] += 1
    end = 'NUDEND-%d' % seq[0]
    m = mark()
    p.stdin.write(('%s; echo NUDEND-$((%d+0))\r' % (c, seq[0])).encode()); p.stdin.flush()
    t0 = time.time()
    while time.time() - t0 < tmo and p.poll() is None:
        o = out(m).replace('\r', '')
        if re.search(r'(?m)^' + end, o): return o
        time.sleep(0.1)
    return None

def ssh(c, tmo=120):
    try:
        r = subprocess.run(['ssh', '-p', str(P_SSH), '-i', a.key, '-o', 'IdentitiesOnly=yes',
                            '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null',
                            '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=30', '-o', 'LogLevel=ERROR',
                            'root@127.0.0.1', c], capture_output=True, text=True, timeout=tmo)
        return r.returncode, r.stdout
    except subprocess.TimeoutExpired:
        return -1, ''

def qmp(c):
    s = socket.socket(socket.AF_UNIX); s.connect(sock)
    f = s.makefile('rw'); f.readline()
    for m in ({'execute': 'qmp_capabilities'}, {'execute': c}):
        f.write(json.dumps(m) + '\n'); f.flush()
        while True:
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r: break
    s.close()

def finish():
    d = dlog()
    for r in KERNEL_BAD:
        check('no /%s/' % r, re.search(r, d) is None)
    for r in a.expect:
        check('expect /%s/' % r, re.search(r, d) is not None)
    try: qmp('quit')
    except Exception: pass
    try: p.wait(15)
    except subprocess.TimeoutExpired:
        try: os.kill(int(open(pidf).read().strip()), 9)
        except Exception: pass
    say('PASS' if not fails else 'FAIL')
    sys.exit(0 if not fails else 1)

def udrv_state():
    o = run('cat /proc/udrv') or ''
    st = {}
    m = re.search(r'live_dma_pages=(\d+) live_bar_pages=(\d+) live_net_pages=(\d+) net_waiting=(\d+)', o)
    if m: st.update(dma=int(m[1]), bar=int(m[2]), net=int(m[3]), waiting=int(m[4]))
    m = re.search(r'net eth0 dev=\S+ (attached pid=(\d+)|waiting) link=(\d) attaches=(\d+) deaths=(\d+)', o)
    if m:
        st.update(pid=int(m[2]) if m[2] else 0, link=int(m[3]), attaches=int(m[4]), deaths=int(m[5]))
    m = re.search(r'bad_total=(\d+)', o)
    if m: st['bad'] = int(m[1])
    return st

def mem():
    o = run("for i in 1 2 3; do grep -E '^(MemFree|Slab|SUnreclaim):' /proc/meminfo | tr -s ' ' | "
            "tr '\\n' ' '; echo; sleep 1; done") or ''
    fr, hp = 0, 1 << 60
    for ln in o.splitlines():
        v = dict(re.findall(r'(MemFree|Slab|SUnreclaim): (\d+)', ln))
        if len(v) == 3:
            fr = max(fr, int(v['MemFree']) + int(v['Slab'])); hp = min(hp, int(v['SUnreclaim']))
    return fr, hp

def download(path, tmo=300):
    o = run('rm -f /tmp/dl; wget -q -O /tmp/dl http://10.0.2.2:%d%s; md5sum /tmp/dl; rm -f /tmp/dl' % (P_HTTP, path), tmo) or ''
    return MD5[path] in o

def wait_attach(old_pid, tmo):
    t0 = time.time()
    while time.time() - t0 < tmo:
        st = udrv_state()
        if st.get('pid', 0) not in (0, old_pid) and st.get('link') == 1: return st, time.time() - t0
        time.sleep(0.3)
    return udrv_state(), None

def timed(c, tmo=600):
    o = run('S=$(cat /proc/uptime); %s; E=$(cat /proc/uptime); echo "T $S $E"' % c, tmo) or ''
    m = re.search(r'T ([\d.]+) \S+ ([\d.]+)', o)
    return (float(m[2]) - float(m[1])) if m else None, o

try:
    if not wait_prompt(a.timeout):
        check('boot-to-bash', False); finish()
    check('boot-to-bash', True)
    d = dlog()
    if a.mode == 'udrv':
        st = {}
        t0 = time.time()
        while time.time() - t0 < 30:
            st = udrv_state()
            if st.get('link') == 1 and st.get('pid'): break
            time.sleep(1)
        check('proxy-attached', st.get('link') == 1 and st.get('pid', 0) > 0 and st.get('waiting') == 0, str(st))
        check('claim-translated', re.search(r'\[UDRV\] claim pid=\d+ bdf=\S+ ok fd=\d+ iommu=translated dma=yes', d) is not None)
        check('no-in-kernel-e1000e', '[E1000E] ready' not in d)
        check('msix', re.search(r'\[UDRV\] irq_setup pid=\d+ bdf=\S+ msix nvec=1', d) is not None)
    else:
        check('in-kernel-e1000e', '[E1000E] ready eth0' in d)
        check('no-udrv', '[UDRV]' not in d)
    o = run('ifconfig eth0') or ''
    check('dhcp-bound', 'inet 10.0.2.15' in o and 'dhcp' in o, o.strip().replace('\n', ' | ')[:200])
    check('tcp-download-8M-md5', download('/f8'))
    n0 = len(sink_md5)
    # Upload the 8 MiB payload back (fetched again; /dev/urandom is slow
    # under TCG): the host sink must see exactly its bytes.
    run('wget -q -O /tmp/up http://10.0.2.2:%d/f8; nc -w 3 10.0.2.2 %d < /tmp/up; rm -f /tmp/up' % (P_HTTP, P_SINK), 300)
    t0 = time.time()
    while len(sink_md5) == n0 and time.time() - t0 < 30: time.sleep(0.2)
    ok = len(sink_md5) > n0 and sink_md5[-1] == (len(PAYLOAD), MD5['/f8'])
    check('tcp-upload-8M-md5', ok, str(sink_md5[-1:]))
    o = run('nslookup localhost 10.0.2.3', 60) or ''
    check('dns', re.search(r'Address( 1)?:?\s+127\.0\.0\.1', o) is not None, o.strip().replace('\n', ' | ')[:200])
    rc, so = ssh('echo NUD-SSH-OK; cat /proc/udrv | head -n1')
    check('ssh-session', rc == 0 and 'NUD-SSH-OK' in so, 'rc=%d' % rc)

    if a.perf:
        t, _ = timed('wget -q -O /dev/null http://10.0.2.2:%d/f32' % P_HTTP)
        say('perf download 32 MiB %.2f s = %.1f Mbit/s' % (t, 32 * 8.388608 / t) if t else 'perf download failed')
        run('head -c 16777216 /dev/zero > /tmp/up16')
        t, _ = timed('nc -w 1 10.0.2.2 %d < /tmp/up16' % P_SINK)
        say('perf upload 16 MiB %.2f s = %.1f Mbit/s' % (t, 16 * 8.388608 / t) if t else 'perf upload failed')
        t, _ = timed('i=0; while [ $i -lt 200 ]; do echo x | nc 10.0.2.2 %d >/dev/null; i=$((i+1)); done' % P_ECHO)
        say('perf 200 TCP connect+echo round trips %.2f s = %.1f ms each' % (t, t * 5) if t else 'perf echo failed')

    if a.mode == 'udrv' and a.kills:
        # The root is ramfs: every command run, log line and /tmp file costs
        # frames (MemFree+Slab drifts ~50 kB per program run, see
        # tools/udrv_check.py). A control phase runs the same commands without
        # the kill, so the leak check compares the two drifts.
        run('(while :; do wget -q -O /dev/null http://10.0.2.2:%d/f8; done) >/dev/null 2>&1 & echo $! > /tmp/loop.pid' % P_HTTP)
        frc0, _ = mem()
        for k in range(a.kills):
            udrv_state(); time.sleep(1.0); run('true')
            for _ in range(3): udrv_state()
            download('/f64k', 120)
        frc1, _ = mem()
        base_st = udrv_state()
        fr0, hp0 = mem()
        times, bad = [], 0
        for k in range(a.kills):
            st = udrv_state()
            pid = st.get('pid', 0)
            if not pid: bad += 1; time.sleep(2); continue
            time.sleep(1.0)
            run('kill -9 %d' % pid)
            st2, dt = wait_attach(pid, RESPAWN_S + 5)
            if dt is None or dt > RESPAWN_S: bad += 1
            else: times.append(dt)
            ok = download('/f64k', 120)
            if not ok: bad += 1
            say('kill %d: pid %d -> %s in %s s, traffic %s' % (k + 1, pid, st2.get('pid'),
                '%.1f' % dt if dt is not None else '-', 'ok' if ok else 'FAILED'))
        run('kill $(cat /tmp/loop.pid); sleep 1')
        check('kill-loop', bad == 0, '%d kills, %d problems, respawn %.1f-%.1f s' %
              (a.kills, bad, min(times) if times else 0, max(times) if times else 0))
        dd = dlog()
        died = len(re.findall(r'\[UDRV\] eth0 driver died pid=\d+ \((exit|reap)\)', dd))
        check('died-lines', died >= a.kills, 'found %d' % died)
        time.sleep(3)
        fr1, hp1 = mem()
        st = udrv_state()
        ctrl = frc0 - frc1
        check('memfree+slab-steady', (fr0 - fr1) - max(ctrl, 0) <= FRAME_SLACK_KB,
              'kill phase: before %d after %d kB (drift %d); control phase drift %d kB' % (fr0, fr1, fr0 - fr1, ctrl))
        check('heap-steady', hp1 - hp0 <= HEAP_SLACK_KB, 'before %d after %d kB' % (hp0, hp1))
        check('ring3-pages-equal', all(st.get(k) == base_st.get(k) for k in ('dma', 'bar', 'net')),
              'before %s after %s' % (base_st, st))
        check('no-bad-entries', st.get('bad', 0) == 0, str(st.get('bad')))
        check('tcp-download-after-kills', download('/f8'))
    finish()
except SystemExit:
    raise
except Exception as e:
    check('exception', False, repr(e))
    finish()
