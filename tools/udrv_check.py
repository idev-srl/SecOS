#!/usr/bin/env python3
"""tools/udrv_check.py — one guest of the M57 Driver Space v2 gate
(tools/udrv_gate.sh).

Boots a SecOS image built with CONSOLE=root (bash on the serial console) on a
q35 machine with QEMU's edu device and runs one scenario:

  --mode run       /bin/udrv_edu runs from bash and passes (claim, BAR,
                   IRQ round trips, DMA through the IOMMU); then --leak-runs
                   more short runs (every other one exits without RELEASE,
                   holding DMA, BAR and IRQ), after which PMM frames (MemFree + Slab),
                   kernel heap in use (SUnreclaim), free IRQ vectors
                   (/proc/interrupts), IOMMU domains and VT-d table pages
                   (/proc/iommu) and the pages ring-3 drivers hold
                   (/proc/udrv) must be back at the level of the first run; /proc/udrv shows the
                   device free with claims == runs, faults 0, /proc/iommu the
                   device blocked and 0 faults;
  --mode service   /bin/udrv_edu --serve runs from /etc/inittab (respawn):
                   it passes, then holds a claim; it is killed with kill -9
                   --kills times, each time the kernel releases the claim
                   ("[UDRV] release pid=<old>") and init restarts it (a new
                   pid passes again and serves); resources as in "run" after
                   the last restart;
  --mode nommu     plain q35, no IOMMU: the claim is granted for MMIO only,
                   DMA_ALLOC is -ENODEV (19), bus mastering and IRQ_SETUP
                   -EPERM (1), the BAR still works;
  --mode default   an image without udrv=: no [UDRV] line at boot,
                   /proc/udrv lists no device, the edu function is in the
                   PCI registry and nobody claims it;
  --mode negtest   only the argument-validation table (below) plus one
                   udrv_edu run: --env noir (IOMMU without interrupt
                   remapping: MMIO only, DMA and bus mastering refused) or
                   --env unsafe (no IOMMU, udrv.unsafe: DMA allowed, the
                   kernel reports the taint).

--negtest ENV (every mode): /bin/udrv_negtest --env ENV, the SYS_UDRV
argument-validation table (user/udrv_negtest.c), must pass every row, and so
must /bin/udrv_user (same table, plain non-driver manifest); afterwards the
device is free, nothing is held and no DMA fault was taken.

Always: no CPU exception, panic, lockdep report or nested fault in the log.
Prints "[udrv] <name>: <check> ok|FAIL ..." and "[udrv] <name>: PASS|FAIL".
Files in --workdir: NAME.log (debugcon), NAME.ser (serial), NAME.err.
QEMU is stopped through QMP, else via its pid file.
SPDX-License-Identifier: MIT
"""
import argparse, json, os, re, socket, subprocess, sys, threading, time

ap = argparse.ArgumentParser()
ap.add_argument('--name', required=True)
ap.add_argument('--workdir', required=True)
ap.add_argument('--qemu', default='qemu-system-x86_64')
ap.add_argument('--mode', required=True, choices=['run', 'service', 'nommu', 'default', 'negtest'])
ap.add_argument('--timeout', type=float, default=600, help='boot timeout (s)')
ap.add_argument('--rounds', type=int, default=10000, help='IRQ round trips of the main run')
ap.add_argument('--leak-runs', type=int, default=8, help='extra short runs for the leak check')
ap.add_argument('--kills', type=int, default=3, help='service mode: kill -9 / restart cycles')
ap.add_argument('--claim-ir', default='on', choices=['on', 'noir'],
                help='--mode run: the claim line of an IR-covered function (on) or of the udrv.noir override (noir)')
ap.add_argument('--negtest', default='', help='environment for udrv_negtest (translated|unsafe|noir|nommu|nodev)')
ap.add_argument('--expect', action='append', default=[])
ap.add_argument('--absent', action='append', default=[])
ap.add_argument('qemu_args', nargs=argparse.REMAINDER)
a = ap.parse_args()
qargs = a.qemu_args[1:] if a.qemu_args[:1] == ['--'] else a.qemu_args

KERNEL_BAD = [r'\[EXC\] int=', r'\[PANIC\]', r'KERNEL PANIC', r'nested fault', r'stack smashing',
              r'LOCKDEP\]', r'\[LOCK\] BUG', r'\[TLB\] BUG', r'shootdown timeout',
              r'invalidation wait TIMEOUT', r'\[SMAP\] violation']
# MemFree+Slab and SUnreclaim drift with bash, the page cache and the klog
# (measured: ~50 kB per run of any program, the same for a run of
# uaccess_test as for udrv_edu), so they get the slack of the other leak gates
# (tools/proc_race_drive.py). The exact checks are the counters below.
FRAME_SLACK_KB = 1024
HEAP_SLACK_KB = 2048

W = a.workdir
os.makedirs(W, exist_ok=True)
log, ser = os.path.join(W, a.name + '.log'), os.path.join(W, a.name + '.ser')
sock, pidf = os.path.join(W, a.name + '.qmp'), os.path.join(W, a.name + '.pid')
for f in (log, ser, sock, pidf):
    try: os.unlink(f)
    except FileNotFoundError: pass

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
    print('[udrv] %s: %s %s %s' % (a.name, name, 'ok' if ok else 'FAIL', detail), flush=True)
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
    """Run c in bash; the output (CR stripped), or None on timeout."""
    seq[0] += 1
    sent = 'UDRVCHK-END-%d' % seq[0]
    m = mark()
    send('%s; echo UDRVCHK-END-$((%d+0))' % (c, seq[0]))
    t0 = time.time()
    while time.time() - t0 < tmo and p.poll() is None:
        o = out(m).replace('\r', '')
        if re.search(r'(?m)^' + sent, o): return o
        time.sleep(0.1)
    hang_dump(c)
    return None

dumped = [False]
def hang_dump(c):
    """First command timeout: save the vCPUs' state to NAME.hang (diagnosis)."""
    if dumped[0] or p.poll() is not None: return
    dumped[0] = True
    try:
        with open(os.path.join(W, a.name + '.hang'), 'w') as f:
            f.write('timeout running: %s\n' % c)
            for _ in range(3):
                for hc in ('info cpus', 'info registers -a'):
                    r = qmp('human-monitor-command', {'command-line': hc})
                    f.write('== %s\n%s\n' % (hc, r.get('return', r)))
                time.sleep(1)
        print('[udrv] %s: command timed out, vCPU state in %s.hang' % (a.name, a.name), flush=True)
    except Exception as e:
        print('[udrv] %s: hang dump failed: %s' % (a.name, e), flush=True)

def qmp(c, args=None):
    s = socket.socket(socket.AF_UNIX); s.connect(sock)
    f = s.makefile('rw'); f.readline()
    for m in ({'execute': 'qmp_capabilities'}, dict({'execute': c}, **({'arguments': args} if args else {}))):
        f.write(json.dumps(m) + '\n'); f.flush()
        while True:
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r: break
    s.close()
    return r

def finish():
    d = dlog()
    for r in KERNEL_BAD:
        check('no /%s/' % r, re.search(r, d) is None)
    # No process killed by a fault; in service mode only our own kill -9s.
    kills = re.findall(r'\[KILL\] pid=0x[0-9a-f]+ sig=0x([0-9a-f]+)', d)
    bad = [k for k in kills if a.mode != 'service' or int(k, 16) != 9]
    check('no-fault-kills', not bad and (a.mode != 'service' or len(kills) == a.kills), 'kills %s' % kills)
    for r in a.expect:
        check('expect /%s/' % r, re.search(r, d) is not None)
    for r in a.absent:
        check('absent /%s/' % r, re.search(r, d) is None)
    try: qmp('quit')
    except Exception: pass
    try: p.wait(15)
    except subprocess.TimeoutExpired:
        try: os.kill(int(open(pidf).read().strip()), 9)
        except Exception: pass
    print('[udrv] %s: %s' % (a.name, 'PASS' if not fails else 'FAIL'), flush=True)
    sys.exit(0 if not fails else 1)

def resources():
    """A dict of resource levels, or None on a timeout:
    frames  MemFree+Slab kB (best of 3 samples: frames outside the heap),
    heap    SUnreclaim kB (best of 3: bytes in heap blocks),
    vectors free IRQ vectors (/proc/interrupts),
    domains IOMMU domains, tables VT-d table pages (/proc/iommu),
    dma/bar pages held by ring-3 drivers (/proc/udrv)."""
    o = run("for i in 1 2 3; do echo MF-$(grep -E '^(MemFree|Slab|SUnreclaim):' /proc/meminfo | "
            "tr -s ' ' | cut -d' ' -f2 | tr '\\n' '-')KB; sleep 1; done; grep '^spurious' /proc/interrupts; "
            "grep -E '^(domains|tables live)' /proc/iommu; head -1 /proc/udrv; echo", 60)
    if o is None: return None
    m = re.findall(r'MF-(\d+)-(\d+)-(\d+)-KB', o)
    v = re.search(r'spurious \d+ free (\d+)', o)
    dm = re.search(r'domains (\d+)', o)
    tb = re.search(r'tables live (\d+)', o)
    lv = re.search(r'live_dma_pages=(\d+) live_bar_pages=(\d+)', o)
    if not m or not v or not lv: return None
    return {'frames': max(int(x) + int(y) for x, y, _ in m), 'heap': min(int(z) for _, _, z in m),
            'vectors': int(v.group(1)), 'domains': int(dm.group(1)) if dm else -1,
            'tables': int(tb.group(1)) if tb else -1, 'dma': int(lv.group(1)), 'bar': int(lv.group(2))}

def compare(tag, base, now):
    """Exact for the counters the driver's resources live in; the PMM and
    heap totals (which move with bash, the page cache and the klog) within
    the slack the other leak gates use."""
    if base is None or now is None:
        check('%s-resources' % tag, False, 'no sample (base %s now %s)' % (base, now)); return
    for k, what in (('vectors', 'free IRQ vectors'), ('domains', 'IOMMU domains'),
                    ('tables', 'VT-d table pages'), ('dma', 'ring-3 DMA pages'), ('bar', 'ring-3 BAR pages')):
        check('%s-%s' % (tag, k), now[k] == base[k], '%s %d -> %d' % (what, base[k], now[k]))
    check('%s-pmm-frames' % tag, base['frames'] - now['frames'] <= FRAME_SLACK_KB,
          'MemFree+Slab %d -> %d kB (slack %d)' % (base['frames'], now['frames'], FRAME_SLACK_KB))
    check('%s-heap-in-use' % tag, now['heap'] - base['heap'] <= HEAP_SLACK_KB,
          'SUnreclaim %d -> %d kB (slack %d)' % (base['heap'], now['heap'], HEAP_SLACK_KB))

def edu_result(o, what):
    """Check one udrv_edu run's output."""
    ok = o is not None and re.search(r'\[udrv_edu\] RESULT pass=\d+ fail=0', o) is not None and \
        '[udrv_edu] PASS' in o
    fl = re.findall(r'\[udrv_edu\] [^\n]* FAIL[^\n]*', o or '')
    r = re.search(r'\[udrv_edu\] RESULT [^\n]*', o or '')
    check(what, ok, (r.group(0) if r else 'timeout/no RESULT') + ('; ' + ' | '.join(fl[:4]) if fl else ''))
    return ok

def proc_udrv():
    return run('cat /proc/udrv', 30) or ''

def table_result(o, what):
    """Check one udrv_negtest / udrv_user run: every row PASS."""
    rows = re.findall(r'\[udrv_negtest\] ([AB]\d\d) [^\n]*: (PASS|FAIL|SKIP)', o or '')
    r = re.search(r'\[udrv_negtest\] RESULT rows=(\d+) pass=(\d+) fail=(\d+) skip=(\d+)', o or '')
    fl = re.findall(r'\[udrv_negtest\] [^\n]*: FAIL[^\n]*', o or '')
    ok = r is not None and r.group(3) == '0' and '[udrv_negtest] PASS' in (o or '') and len(rows) == int(r.group(1))
    check(what, ok, (r.group(0) if r else 'timeout/no RESULT') + ('; ' + ' | '.join(fl[:6]) if fl else ''))
    return ok

def negtest(env):
    """The argument-validation table as a driver and as a plain program."""
    o = run('/bin/udrv_negtest --env %s' % env, 300)
    table_result(o, 'negtest-%s' % env)
    for l in re.findall(r'\[udrv_negtest\] (?:RESULT|env )[^\n]*', o or ''):
        print('[udrv] %s: %s' % (a.name, l), flush=True)
    uenv = 'nodev' if env == 'nodev' else 'user'
    o = run('/bin/udrv_user --env %s' % uenv, 120)
    table_result(o, 'negtest-udrv_user-%s' % uenv)
    if env == 'nodev': return
    d = dlog()
    check('negtest-log-not-root', re.search(r'\[UDRV\] claim pid=\d+ bdf=00:04\.0 refused\(not-root\)', d) is not None)
    check('negtest-log-not-a-driver', re.search(r'\[UDRV\] claim pid=\d+ bdf=00:04\.0 refused\(not-a-driver\)', d) is not None)
    check('negtest-log-busy', re.search(r'\[UDRV\] claim pid=\d+ bdf=00:04\.0 refused\(busy\)', d) is not None)
    o = proc_udrv()
    check('negtest-device-free', re.search(r'dev 00:04\.0 1234:11e8 free claims=\d+ faults=0 kills=0 ', o) is not None and
          re.search(r'live_dma_pages=0 live_bar_pages=0', o) is not None and
          re.search(r' mode=none nvec=0 pending=0x0 masked=0x0 dma=0', o) is not None, repr(o[-300:]))

t_start = time.time()
ok_prompt = wait_prompt(a.timeout)
check('boot-to-bash', ok_prompt, '(%.0fs)' % (time.time() - t_start))
if not ok_prompt: finish()
time.sleep(1)
d = dlog()

if a.mode == 'default':
    check('no-udrv-lines', re.search(r'\[UDRV\]', d) is None,
          ' | '.join(re.findall(r'\[UDRV\][^\n]*', d)[:3]))
    o = proc_udrv()
    check('proc-udrv-empty', re.search(r'udrv devices 0 ', o) is not None and 'dev ' not in o, repr(o[-200:]))
    o = run('grep -i 11e8 /proc/bus/pci/devices', 30) or ''
    check('edu-in-registry', re.search(r'123411e8', o) is not None, repr(o[-120:]))
    if a.negtest: negtest(a.negtest)
    finish()

if a.mode == 'nommu':
    check('assigned-mmio-only', re.search(r'\[UDRV\] udrv= 00:04\.0 1234:11e8 assigned to ring 3 \(no iommu: MMIO only\)', d)
          is not None)
    o = run('/bin/udrv_edu -n 10', 120)
    edu_result(o, 'udrv_edu-mmio-pass')
    for c in ('id-register', 'liveness', 'factorial', 'mmio-only-refusals', 'release'):
        check('edu-' + c, o is not None and re.search(r'\[udrv_edu\] %s ok' % c, o) is not None)
    m = re.search(r'mmio-only: dma_alloc rc=(-?\d+)', o or '')
    check('dma-alloc-enodev', m is not None and m.group(1) == '-19', m.group(0) if m else '')
    d = dlog()
    check('log-claim-no-dma', re.search(r'\[UDRV\] claim pid=\d+ bdf=00:04\.0 ok fd=\d+ iommu=none dma=no', d) is not None)
    check('log-dma-refused', re.search(r'\[UDRV\] dma_alloc pid=\d+ bdf=00:04\.0 refused\(no IOMMU\)', d) is not None)
    check('log-bm-refused', re.search(r'\[UDRV\] cfg_write pid=\d+ bdf=00:04\.0 refused\(busmaster: no IOMMU\)', d) is not None)
    o = proc_udrv()
    check('proc-udrv-free', re.search(r'dev 00:04\.0 1234:11e8 free claims=1 faults=0', o) is not None, repr(o[-240:]))
    if a.negtest: negtest(a.negtest)
    finish()

if a.mode == 'negtest':
    o = run('/bin/udrv_edu -n 100', 300)
    edu_result(o, 'udrv_edu-pass')
    d = dlog()
    if a.negtest == 'unsafe':
        check('log-claim-tainted', re.search(r'\[UDRV\] claim pid=\d+ bdf=00:04\.0 ok fd=\d+ iommu=none dma=yes TAINTED\(unsafe\)', d)
              is not None)
        check('log-taint', '[UDRV] WARNING: TAINTED: ring-3 DMA without IOMMU (udrv.unsafe)' in d)
        check('edu-dma-data', o is not None and re.search(r'\[udrv_edu\] dma-data ok', o) is not None)
        o = proc_udrv()
        check('proc-udrv-tainted', 'TAINTED: ring-3 DMA without IOMMU' in o, repr(o[:200]))
    if a.negtest == 'noir':
        check('log-claim-no-ir', re.search(r'\[UDRV\] claim pid=\d+ bdf=00:04\.0 ok fd=\d+ iommu=translated dma=no', d) is not None)
        check('log-bm-refused-no-ir', re.search(r'\[UDRV\] cfg_write pid=\d+ bdf=00:04\.0 refused\(busmaster: no interrupt remapping\)', d)
              is not None)
        check('log-dma-refused-no-ir', re.search(r'\[UDRV\] dma_alloc pid=\d+ bdf=00:04\.0 refused\(no interrupt remapping\)', d)
              is not None)
    negtest(a.negtest)
    finish()

def common_after(tag, claims):
    o = proc_udrv()
    print('[udrv] %s: /proc/udrv\n%s' % (a.name, o.strip()), flush=True)
    check('%s-proc-udrv' % tag, re.search(r'dev 00:04\.0 1234:11e8 free claims=%d faults=0 kills=0 ' % claims, o)
          is not None and re.search(r' mode=none nvec=0 pending=0x0 masked=0x0 dma=0', o) is not None, repr(o[-300:]))
    o = run('cat /proc/iommu', 30) or ''
    check('%s-proc-iommu' % tag, re.search(r'dev 00:04\.0 1234:11e8 domain blocked', o) is not None and
          re.search(r'faults 0 irqs 0', o) is not None, repr(o[-300:]))

if a.mode == 'run':
    check('assigned', re.search(r'\[UDRV\] udrv= 00:04\.0 1234:11e8 assigned to ring 3 \(iommu: blocked\)', d) is not None)
    base0 = resources()
    o = run('/bin/udrv_edu -n %d' % a.rounds, 900)
    edu_result(o, 'udrv_edu-pass')
    for l in re.findall(r'\[udrv_edu\] (?:irq-rounds|dma-data|dma-alloc|id-register|factorial)[^\n]*', o or ''):
        print('[udrv] %s: %s' % (a.name, l), flush=True)
    d = dlog()
    # [M56] With interrupt remapping covering edu the claim line ends at
    # dma=yes; only the udrv.noir override (IR off) adds ir=no(udrv.noir).
    claim_re = (r'\[UDRV\] claim pid=\d+ bdf=00:04\.0 ok fd=\d+ iommu=translated dma=yes ir=no\(udrv\.noir\)'
                if a.claim_ir == 'noir' else
                r'(?m)\[UDRV\] claim pid=\d+ bdf=00:04\.0 ok fd=\d+ iommu=translated dma=yes\r?$')
    check('log-claim', re.search(claim_re, d) is not None)
    check('log-irq-setup', re.search(r'\[UDRV\] irq_setup pid=\d+ bdf=00:04\.0 msi nvec=1', d) is not None)
    check('log-release', re.search(r'\[UDRV\] release pid=\d+ bdf=00:04\.0 \(release\)', d) is not None)
    m = re.search(r'dma-alloc ok rc=0 uva=0x[0-9a-f]+ iova=(0x[0-9a-f]+)', o or '')
    print('[udrv] %s: iova %s' % (a.name, m.group(1) if m else '?'), flush=True)
    base = resources()
    for i in range(a.leak_runs):
        # Every other run exits holding its claim (a DMA buffer, the BAR, an
        # armed vector): the exit path, not RELEASE, must give it all back.
        hold = ' --hold' if i % 2 else ''
        o = run('/bin/udrv_edu -n 50' + hold, 300)
        if not edu_result(o, 'leak-run-%d%s' % (i + 1, hold.replace(' --', '-'))): break
        if hold:
            check('leak-run-%d-held' % (i + 1), o is not None and
                  re.search(r'\[udrv_edu\] holding pid=\d+ fd=\d+ bar=0 bm=0 irq=0 dma=0', o) is not None)
    time.sleep(2)
    now = resources()
    print('[udrv] %s: resources before %s, after run 1 %s, after %d more %s' % (a.name, base0, base, a.leak_runs, now),
          flush=True)
    compare('runs', base, now)
    compare('boot', base0, now)
    check('runs-nothing-held', now is not None and now['dma'] == 0 and now['bar'] == 0,
          'dma %s bar %s' % (now and now['dma'], now and now['bar']))
    common_after('runs', 1 + a.leak_runs + a.leak_runs // 2)   # a --hold run claims twice
    if a.negtest:
        negtest(a.negtest)
        time.sleep(2)
        after = resources()
        print('[udrv] %s: resources after the negative table %s' % (a.name, after), flush=True)
        compare('negtest', now, after)
        o = run('cat /proc/iommu', 30) or ''
        check('negtest-iommu-no-faults', re.search(r'dev 00:04\.0 1234:11e8 domain blocked', o) is not None and
              re.search(r'faults 0 irqs 0', o) is not None, repr(o[-300:]))
    finish()

if a.mode == 'service':
    def svc_log():
        return run('cat /var/log/edu.log', 30) or ''
    def wait_serving(n, tmo=300):
        t0 = time.time()
        while time.time() - t0 < tmo:
            lg = svc_log()
            if len(re.findall(r'\[udrv_edu\] serving pid=\d+ fd=\d+ bar=0 bm=0 irq=0 dma=0', lg)) >= n: return lg
            time.sleep(3)
        return svc_log()
    lg = wait_serving(1)
    edu_result(lg, 'service-first-pass')
    pids = re.findall(r'\[udrv_edu\] serving pid=(\d+)', lg)
    check('service-serving', len(pids) >= 1, repr(lg[-200:]))
    base = None
    for k in range(a.kills):
        o = proc_udrv()
        m = re.search(r'dev 00:04\.0 1234:11e8 claimed pid=(\d+)', o)
        check('kill%d-claimed' % (k + 1), m is not None, repr(o[-200:]))
        if not m: break
        if k == 1: base = resources()       # after one restart: warm
        victim = m.group(1)
        run('kill -9 %s' % victim, 30)
        lg = wait_serving(k + 2)
        n_pass = len(re.findall(r'\[udrv_edu\] PASS pid=\d+', lg))
        new = re.findall(r'\[udrv_edu\] serving pid=(\d+)', lg)
        check('kill%d-restarted' % (k + 1), len(new) >= k + 2 and new[-1] != victim and n_pass >= k + 2,
              'victim %s serving %s passes %d' % (victim, new, n_pass))
        check('kill%d-released' % (k + 1),
              re.search(r'\[UDRV\] release pid=%s bdf=00:04\.0 \((exit|reap)\)' % victim, dlog()) is not None)
        if not (len(new) >= k + 2): break
    fl = re.findall(r'\[udrv_edu\] [^\n]* FAIL[^\n]*', svc_log())
    check('service-no-fail-lines', not fl, ' | '.join(fl[:3]))
    now = resources()
    print('[udrv] %s: resources after kill 2 %s, after kill %d %s' % (a.name, base, a.kills, now), flush=True)
    compare('kills', base, now)
    finish()
