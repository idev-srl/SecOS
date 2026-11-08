#!/usr/bin/env python3
"""tools/pci_check.py — M53a PCI registry check against QEMU's own view.

Boots one QEMU guest, waits until the kernel has probed its drivers, asks QEMU
for its PCI tree over QMP (query-pci) and compares it with the kernel's
"[PCI] bb:dd.f vvvv:dddd class cccccc ..." boot lines (drivers/pci_registry.c).

usage: pci_check.py --name N --workdir DIR [--qemu BIN] [--timeout S]
                    [--wait REGEX] [--expect-ecam on|off|any] -- QEMU ARGS...

Checks: every function QEMU reports is in the registry with the same ids and
class and vice versa; the ECAM state matches --expect-ecam; no CPU exception,
panic or lockdep report. Exit 0 = PASS. Files: DIR/N.log (debugcon),
DIR/N.qmp.json. QEMU is stopped through QMP, or by the pid in DIR/N.pid.
"""
import argparse, json, os, re, signal, socket, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument('--name', required=True)
ap.add_argument('--workdir', required=True)
ap.add_argument('--qemu', default='qemu-system-x86_64')
ap.add_argument('--timeout', type=float, default=180)
ap.add_argument('--wait', default=r'\[NET\] (netcfg|no NIC)|\[NET\] up|login|bash-')
ap.add_argument('--expect-ecam', default='any', choices=['on', 'off', 'any'])
ap.add_argument('qemu_args', nargs=argparse.REMAINDER)
a = ap.parse_args()
qargs = a.qemu_args[1:] if a.qemu_args[:1] == ['--'] else a.qemu_args

os.makedirs(a.workdir, exist_ok=True)
log = os.path.join(a.workdir, a.name + '.log')
sock = os.path.join(a.workdir, a.name + '.qmp')
pidf = os.path.join(a.workdir, a.name + '.pid')
for f in (log, sock, pidf):
    try:
        os.unlink(f)
    except FileNotFoundError:
        pass

cmd = [a.qemu, '-display', 'none', '-no-reboot', '-serial', 'none',
       '-debugcon', 'file:' + log, '-qmp', 'unix:%s,server,nowait' % sock,
       '-pidfile', pidf] + qargs
err = open(os.path.join(a.workdir, a.name + '.err'), 'w')
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err)

def text():
    try:
        return open(log, errors='replace').read()
    except FileNotFoundError:
        return ''

t0 = time.time()
reached = False
while time.time() - t0 < a.timeout and p.poll() is None:
    if re.search(a.wait, text()):
        reached = True
        break
    time.sleep(1)

qmp = None
if p.poll() is None:
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(sock)
        f = s.makefile('rw')
        f.readline()

        def q(c):
            f.write(json.dumps({'execute': c}) + '\n')
            f.flush()
            while True:
                r = json.loads(f.readline())
                if 'return' in r or 'error' in r:
                    return r
        q('qmp_capabilities')
        qmp = q('query-pci').get('return')
        json.dump(qmp, open(os.path.join(a.workdir, a.name + '.qmp.json'), 'w'), indent=1)
        q('quit')
    except Exception as e:  # fall through to the pid-file kill
        print('[pci] %s: QMP error %s' % (a.name, e))
try:
    p.wait(15)
except subprocess.TimeoutExpired:
    try:
        os.kill(int(open(pidf).read().strip()), signal.SIGKILL)
    except Exception:
        p.kill()
    p.wait()

fail = []
if not reached:
    fail.append('boot did not reach /%s/ in %.0fs' % (a.wait, a.timeout))
if qmp is None:
    fail.append('no QMP query-pci data (qemu rc=%s, see %s.err)' % (p.returncode, a.name))

def flat(devs, out):
    for d in devs:
        out[(d['bus'], d['slot'], d['function'])] = (
            d['id']['vendor'], d['id']['device'], d['class_info']['class'])
        br = d.get('pci_bridge')
        if br and br.get('devices'):
            flat(br['devices'], out)

want = {}
for b in qmp or []:
    flat(b['devices'], want)
got = {}
t = text()
for m in re.finditer(r'\[PCI\] ([0-9a-f]{2}):([0-9a-f]{2})\.(\d) ([0-9a-f]{4}):([0-9a-f]{4}) '
                     r'class ([0-9a-f]{4})', t):
    got[(int(m[1], 16), int(m[2], 16), int(m[3]))] = (
        int(m[4], 16), int(m[5], 16), int(m[6], 16))
if qmp is not None:
    for k in sorted(set(want) | set(got)):
        if want.get(k) != got.get(k):
            fail.append('%02x:%02x.%d qemu=%s kernel=%s' % (k[0], k[1], k[2], want.get(k), got.get(k)))

ecam_ok = '[PCI] ecam self-check OK' in t
reg = re.search(r'\[PCI\] registry: .*', t)
if not reg:
    fail.append('no "[PCI] registry" line')
if a.expect_ecam == 'on' and not ecam_ok:
    fail.append('expected "[PCI] ecam self-check OK"')
if a.expect_ecam == 'off' and (ecam_ok or (reg and 'config ecam' in reg[0])):
    fail.append('ECAM is on, expected CF8/CFC')
for bad in ('[EXC]', 'PANIC', '[LOCKDEP]', '[LOCK] BUG', 'self-check FAIL'):
    if bad in t and not (bad == 'self-check FAIL' and a.expect_ecam == 'off'):
        fail.append('log contains %s' % bad)

print('[pci] %s: qemu %d functions, kernel %d; %s' % (
    a.name, len(want), len(got), reg[0][6:] if reg else '-'))
for f_ in fail:
    print('[pci] %s: FAIL %s' % (a.name, f_))
print('[pci] %s: %s' % (a.name, 'PASS' if not fail else 'FAIL'))
sys.exit(0 if not fail else 1)
