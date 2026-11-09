#!/usr/bin/env python3
"""tools/iommu_check.py — one guest of the M54 VT-d gate (tools/iommu_gate.sh).

Boots a SecOS image built with CONSOLE=root (bash on the serial console),
waits for the prompt and checks:

  --expect RE      the debugcon log must match RE (repeatable);
  --absent RE      ... must not match RE (repeatable);
  --edu            the in-kernel edu tests ran: "[IOMMU-TEST] RESULT pass=N
                   fail=0", and /proc/iommu reports exactly the faults the
                   test caused (none from the real drivers);
  --io LIST        comma list of guest I/O to run through bash:
                     net               the boot DHCP lease, then 1 MiB over HTTP
                                       from a host server and 1 MiB to a host
                                       TCP sink (md5 compared both ways);
                     hid               keys typed into the usb-kbd through QMP
                                       sendkey run a command in bash;
                     proc              /proc/iommu lists the unit and devices;
  --disk dev[:NODE]=IMG   dev = sata (/dev/sda), nvme (/dev/nvme0n1), usb
                   (/dev/usb0), NODE overrides the device node:
                   the host writes 1 MiB of random data at offset 0 of IMG,
                   the guest must read the same md5, then writes 1 MiB at
                   offset 1 MiB that the host checks after QEMU exits;
  --proc-expect RE /proc/iommu must match RE (repeatable; [M55] dma lines);
  --faults N       /proc/iommu must show exactly N faults (default: 0, or the
                   edu test count with --edu; -1 = don't check).

Always: no CPU exception, panic, lockdep report or nested fault in the log.
Prints "[iommu] <name>: <check> ok|FAIL ..." lines and "[iommu] <name>: PASS"
or "... FAIL". Files in --workdir: NAME.log (debugcon), NAME.ser (serial),
NAME.err. QEMU is stopped through QMP, else via its pid file.
SPDX-License-Identifier: MIT
"""
import argparse, json, os, re, socket, subprocess, sys, threading, time

ap = argparse.ArgumentParser()
ap.add_argument('--name', required=True)
ap.add_argument('--workdir', required=True)
ap.add_argument('--qemu', default='qemu-system-x86_64')
ap.add_argument('--timeout', type=float, default=600)
ap.add_argument('--expect', action='append', default=[])
ap.add_argument('--absent', action='append', default=[])
ap.add_argument('--edu', action='store_true')
ap.add_argument('--io', default='')
ap.add_argument('--faults', type=int, default=None)
ap.add_argument('--proc-expect', action='append', default=[])
ap.add_argument('--cmd', action='append', default=[], help='run in bash and print the output (debugging)')
ap.add_argument('--disk', action='append', default=[],
                help='dev=IMAGE: the host fills 1 MiB at offset 0 before the boot; the guest '
                     'must read it back, write 1 MiB at offset 1 MiB, and the host checks it')
ap.add_argument('--proc-off', action='store_true', help='/proc/iommu must say remapping is off')
ap.add_argument('--cmdfile', default=None, help='one --cmd per line')
ap.add_argument('--no-prompt', action='store_true', help='do not wait for bash (log checks only)')
ap.add_argument('qemu_args', nargs=argparse.REMAINDER)
a = ap.parse_args()
if a.cmdfile: a.cmd += [l.rstrip('\n') for l in open(a.cmdfile) if l.strip()]
qargs = a.qemu_args[1:] if a.qemu_args[:1] == ['--'] else a.qemu_args

KERNEL_BAD = [r'\[EXC\] int=', r'\[PANIC\]', r'KERNEL PANIC', r'nested fault', r'stack smashing',
              r'LOCKDEP\]', r'\[LOCK\] BUG', r'\[TLB\] BUG', r'shootdown timeout',
              r'invalidation wait TIMEOUT']

W = a.workdir
os.makedirs(W, exist_ok=True)
log, ser = os.path.join(W, a.name + '.log'), os.path.join(W, a.name + '.ser')
sock, pidf = os.path.join(W, a.name + '.qmp'), os.path.join(W, a.name + '.pid')
for f in (log, ser, sock, pidf):
    try: os.unlink(f)
    except FileNotFoundError: pass

import hashlib, http.server, socketserver
disks = {}
for spec in a.disk:
    dev, img = spec.split('=', 1)
    node = None
    if ':' in dev: dev, node = dev.split(':', 1)
    blob = os.urandom(1 << 20)
    with open(img, 'r+b') as f:
        f.seek(0); f.write(blob)
    disks[dev] = (img, hashlib.md5(blob).hexdigest(), node)

# Host peers for the network check (127.0.0.1 = 10.0.2.2 in the guest; random
# high ports, never 2222/8000): a 1 MiB HTTP payload and a TCP sink.
PAYLOAD = os.urandom(1 << 20)
sink = {'md5': None}
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200); self.send_header('Content-Length', str(len(PAYLOAD))); self.end_headers()
        self.wfile.write(PAYLOAD)
    def log_message(self, *x): pass
httpd = socketserver.TCPServer(('127.0.0.1', 0), H)
threading.Thread(target=httpd.serve_forever, daemon=True).start()
sinks = socket.socket(); sinks.bind(('127.0.0.1', 0)); sinks.listen(1)
def sink_run():
    try:
        # Close after 1 MiB: BusyBox nc keeps the connection open after its
        # stdin EOF until the peer closes.
        c, _ = sinks.accept(); h = hashlib.md5(); n = 0
        while n < (1 << 20):
            b = c.recv(min(65536, (1 << 20) - n))
            if not b: break
            h.update(b); n += len(b)
        sink['md5'] = h.hexdigest(); c.close()
    except Exception:
        pass
threading.Thread(target=sink_run, daemon=True).start()

cmd = [a.qemu, '-display', 'none', '-no-reboot', '-serial', 'stdio',
       '-debugcon', 'file:' + log, '-global', 'isa-debugcon.iobase=0xe9',
       '-qmp', 'unix:%s,server,nowait' % sock, '-pidfile', pidf] + qargs
err = open(os.path.join(W, a.name + '.err'), 'w')
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err)
buf = bytearray(); lk = threading.Lock()
serf = open(ser, 'wb')

def rd():
    while True:
        b = p.stdout.read1(65536)
        if not b: break
        with lk: buf.extend(b)
        serf.write(b); serf.flush()
threading.Thread(target=rd, daemon=True).start()

fails = []
def check(name, ok, detail=''):
    print('[iommu] %s: %s %s %s' % (a.name, name, 'ok' if ok else 'FAIL', detail), flush=True)
    if not ok: fails.append(name)

def dlog():
    try: return open(log, 'rb').read().decode('latin1')
    except OSError: return ''

def out(since):
    with lk: return bytes(buf[since:]).decode('latin1')

def mark():
    with lk: return len(buf)

def send(s):
    p.stdin.write((s + '\r').encode()); p.stdin.flush()

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
    seq[0] += 1
    sent = 'IOMMUCHK-END-%d' % seq[0]
    m = mark()
    send('%s; echo IOMMUCHK-END-$((%d+0))' % (c, seq[0]))
    t0 = time.time()
    while time.time() - t0 < tmo and p.poll() is None:
        o = out(m)
        if re.search(r'(?m)^' + sent, o.replace('\r', '')): return o
        time.sleep(0.1)
    return None

qmpf = [None]
def qmp(cmd, args=None):
    if qmpf[0] is None:
        s = socket.socket(socket.AF_UNIX); s.connect(sock)
        f = s.makefile('rw'); f.readline()
        qmpf[0] = f
        qmp('qmp_capabilities')
    f = qmpf[0]
    m = {'execute': cmd}
    if args: m['arguments'] = args
    f.write(json.dumps(m) + '\n'); f.flush()
    while True:
        r = json.loads(f.readline())
        if 'return' in r or 'error' in r: return r

KEYS = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', '>': None}
def sendkeys(text):
    for ch in text:
        k = KEYS.get(ch, ch)
        if k is None: continue
        qmp('send-key', {'keys': [{'type': 'qcode', 'data': k}]})
        time.sleep(0.05)
    qmp('send-key', {'keys': [{'type': 'qcode', 'data': 'ret'}]})

t_start = time.time()
ok_prompt = True
if not a.no_prompt:
    ok_prompt = wait_prompt(a.timeout)
    check('boot-to-bash', ok_prompt, '(%.0fs)' % (time.time() - t_start))
else:
    # Log-only guest: wait for init (or a fixed time).
    t0 = time.time()
    while time.time() - t0 < a.timeout and p.poll() is None:
        if re.search(r'\[init\] started|\[EXEC\] execve', dlog()): break
        time.sleep(1)

guest_md5 = {}
io = [x for x in a.io.split(',') if x]
if ok_prompt and not a.no_prompt:
    time.sleep(1)

    for dev, name in (('sata', '/dev/sda'), ('nvme', '/dev/nvme0n1'), ('usb', '/dev/usb0')):
        if dev not in disks: continue
        img, hmd5, node = disks[dev]
        name = node or name
        o = run('dd if=%s bs=4096 count=256 2>/dev/null | md5sum' % name, 180)
        good = o is not None and hmd5 in o
        check('io-%s-read' % dev, good, '' if good else repr((o or 'timeout')[-160:]))
        o = run('dd if=/dev/urandom of=/tmp/io.a bs=4096 count=256 2>/dev/null; '
                'dd if=/tmp/io.a of=%s bs=4096 seek=256 count=256 conv=notrunc 2>/dev/null; sync; '
                'md5sum /tmp/io.a' % name, 180)
        mm = re.search(r'([0-9a-f]{32})  /tmp/io.a', o or '')
        check('io-%s-write' % dev, mm is not None, '' if mm else repr((o or 'timeout')[-160:]))
        if mm: guest_md5[dev] = mm.group(1)
    for c in a.cmd:
        o = run(c, 120)
        print('[iommu] %s: $ %s\n%s' % (a.name, c, (o or 'timeout').replace('\r', '')), flush=True)
    if 'net' in io:
        d = dlog()
        lease = re.search(r'\[DHCP\] lease ip=', d) is not None
        if not lease:
            run('ifup -t 15', 60)
            lease = re.search(r'\[DHCP\] lease ip=', dlog()) is not None
        check('net-dhcp', lease)
        hp = httpd.server_address[1]
        o = run('wget -q -O /tmp/net.a http://10.0.2.2:%d/ && md5sum /tmp/net.a' % hp, 120)
        hm = hashlib.md5(PAYLOAD).hexdigest()
        check('net-rx-1MiB', o is not None and hm in o, '' if o and hm in o else repr((o or 'timeout')[-160:]))
        sp = sinks.getsockname()[1]
        o = run('dd if=/dev/urandom of=/tmp/net.b bs=4096 count=256 2>/dev/null; '
                'nc 10.0.2.2 %d < /tmp/net.b; md5sum /tmp/net.b' % sp, 120)
        time.sleep(1)
        mm = re.search(r'([0-9a-f]{32})  /tmp/net.b', o or '')
        check('net-tx-1MiB', mm is not None and sink['md5'] == mm.group(1),
              '' if mm and sink['md5'] == mm.group(1) else 'guest %s host %s' % (mm.group(1) if mm else '?', sink['md5']))
    if 'hid' in io:
        run('rm -f /tmp/hidok', 20)
        try:
            sendkeys('touch /tmp/hidok')
            time.sleep(2)
            o = run('test -e /tmp/hidok && echo HID""-OK', 30)
            check('hid-keyboard', o is not None and 'HID-OK' in o)
        except Exception as e:
            check('hid-keyboard', False, 'QMP: %s' % e)
    o = run('cat /proc/iommu', 30) if ('proc' in io or a.edu or a.faults is not None or a.proc_expect) else None
    if o is not None:
        open(os.path.join(W, a.name + '.proc'), 'w').write(o)
    if 'proc' in io and a.proc_off:
        check('proc-iommu-off', o is not None and re.search(r'iommu off state off', o) is not None,
              '' if o else 'no output')
    elif 'proc' in io:
        check('proc-iommu', o is not None and re.search(r'iommu vt-d state on', o) is not None and
              re.search(r'unit 0 base 0x[0-9a-f]+ translating', o) is not None,
              '' if o else 'no output')
    for r in a.proc_expect:
        check('proc-expect /%s/' % r, o is not None and re.search(r, o) is not None,
              '' if o else 'no output')

d = dlog()
for r in KERNEL_BAD:
    check('no /%s/' % r, re.search(r, d) is None)
for r in a.expect:
    check('expect /%s/' % r, re.search(r, d) is not None)
for r in a.absent:
    check('absent /%s/' % r, re.search(r, d) is None)
want = a.faults
if a.edu:
    m = re.search(r'\[IOMMU-TEST\] RESULT pass=(\d+) fail=(\d+) faults=(\d+)', d)
    check('edu-tests', m is not None and m.group(2) == '0' and int(m.group(1)) > 0,
          m.group(0) if m else 'no RESULT line')
    for l in re.findall(r'\[IOMMU-TEST\] FAIL [^\n]*', d): print('    ' + l)
    if want is None and m: want = int(m.group(3))
if want is None: want = 0
if want >= 0 and not a.no_prompt and ok_prompt:
    o = run('cat /proc/iommu', 30)
    m = re.search(r'faults (\d+) irqs (\d+)', o or '')
    check('fault-count', m is not None and int(m.group(1)) == want,
          ('faults %s want %d' % (m.group(1), want)) if m else 'no /proc/iommu faults line')

try:
    qmp('quit')
except Exception:
    pass
try:
    p.wait(15)
except subprocess.TimeoutExpired:
    try: os.kill(int(open(pidf).read().strip()), 9)
    except Exception: pass
if not a.no_prompt and ok_prompt:
    for dev, (img, _, _) in disks.items():
        if dev not in guest_md5: continue
        with open(img, 'rb') as f:
            f.seek(1 << 20); got = hashlib.md5(f.read(1 << 20)).hexdigest()
        check('io-%s-host-readback' % dev, got == guest_md5[dev], '' if got == guest_md5[dev] else 'host %s guest %s' % (got, guest_md5[dev]))
print('[iommu] %s: %s' % (a.name, 'PASS' if not fails else 'FAIL'), flush=True)
sys.exit(0 if not fails else 1)
