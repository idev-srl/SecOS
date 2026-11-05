#!/usr/bin/env python3
# SecOS - small, idempotent source adjustments applied to the fetched BusyBox
# tree by tools/port-busybox.sh. Each one works around a SecOS ABI difference
# that a header cannot paper over. Usage: bb_patch_src.py <busybox-src-dir>
# SPDX-License-Identifier: MIT
import sys

src = sys.argv[1]
PATCHES = [
    # struct stat (pinned to kernel/secos_abi.h) has whole-second
    # st_atime/st_mtime/st_ctime fields, no struct timespec st_*tim.
    ('coreutils/stat.c', [
        ('human_time(&statbuf->st_atim)', 'human_time(&(struct timespec){ statbuf->st_atime, 0 })'),
        ('human_time(&statbuf->st_mtim)', 'human_time(&(struct timespec){ statbuf->st_mtime, 0 })'),
        ('human_time(&statbuf->st_ctim)', 'human_time(&(struct timespec){ statbuf->st_ctime, 0 })'),
    ]),
    ('coreutils/touch.c', [
        ('stbuf.st_atim.tv_nsec', '0'),
        ('stbuf.st_mtim.tv_nsec', '0'),
    ]),
]

for rel, reps in PATCHES:
    path = '%s/%s' % (src, rel)
    s = open(path).read()
    orig = s
    for old, new in reps:
        s = s.replace(old, new)
    if s != orig:
        open(path, 'w').write(s)
