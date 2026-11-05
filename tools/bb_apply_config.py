#!/usr/bin/env python3
# SecOS - apply the hand-written BusyBox config fragment (user/busybox.config)
# to a BusyBox .config produced by `make defconfig`.
#
#   bb_apply_config.py <fragment> <.config> <busybox-src-dir>
#
# * every "CONFIG_X=v" line of the fragment sets X;
# * every "# CONFIG_X is not set" line clears X;
# * every APPLET (a symbol named in a `//applet:IF_X(` line of the BusyBox
#   sources) that the fragment does not set to y is cleared, so the result is
#   "upstream defaults for the FEATURE_* options of exactly the applets we
#   asked for". Run `make oldconfig` afterwards to settle dependencies.
# SPDX-License-Identifier: MIT
import os
import re
import sys

frag, cfg, src = sys.argv[1], sys.argv[2], sys.argv[3]
want = {}
for line in open(frag):
    line = line.strip()
    m = re.match(r'^(CONFIG_[A-Z0-9_]+)=(.*)$', line)
    if m:
        want[m.group(1)] = m.group(2)
        continue
    m = re.match(r'^# (CONFIG_[A-Z0-9_]+) is not set$', line)
    if m:
        want[m.group(1)] = None

applets = set()
for d, _, files in os.walk(src):
    for f in files:
        if not f.endswith('.c'):
            continue
        try:
            txt = open(os.path.join(d, f), errors='replace').read()
        except OSError:
            continue
        for a in re.findall(r'//applet:IF_([A-Z0-9_]+)\(', txt):
            applets.add('CONFIG_' + a)
for a in applets:
    if want.get(a) != 'y':
        want[a] = None

out, seen = [], set()
for line in open(cfg):
    m = re.match(r'^(?:# )?(CONFIG_[A-Z0-9_]+)(?:=.*| is not set)$', line.rstrip('\n'))
    if m and m.group(1) in want:
        k = m.group(1)
        seen.add(k)
        v = want[k]
        out.append(('# %s is not set\n' % k) if v is None else ('%s=%s\n' % (k, v)))
    else:
        out.append(line)
for k, v in sorted(want.items()):
    if k not in seen and v is not None:
        out.append('%s=%s\n' % (k, v))
open(cfg, 'w').write(''.join(out))
