/*
 * SecOS Kernel - system root provisioning (kernel/sysroot.c)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once

/* Record which root was mounted (kernel_main_phase2). persistent = 1 for a
 * disk root (ext2/ext4 with /.secosroot), 0 for the ramfs fallback. */
void sysroot_set_root(const char* dev, int persistent);
int  sysroot_is_persistent(void);
const char* sysroot_root_dev(void);

/* Install the signed system programs carried in the kernel image and the
 * default configuration (policy in docs/devlog/M41-persistent-root.md):
 *   - system files (/bin programs): refreshed from the kernel when they differ,
 *     tracked in /var/lib/secos/manifest so unchanged files are not rewritten;
 *   - aliases (multi-call names): symlinks on a disk root, copies/stubs on ramfs;
 *   - configuration (/etc/inittab, passwd, bashrc, ...): created only if missing;
 *   - /etc/secos-release: rewritten when the build changes;
 *   - baked SSH keys: appended to /root/.ssh/authorized_keys when missing.
 * Everything else (/root, /home, /var, /tmp, /etc/dropbear) is left alone. */
void sysroot_provision(void);

/* Apply persistent settings read from /etc (keyboard layout from
 * /etc/vconsole.conf KEYMAP=). The build default stays when the file is absent. */
void sysroot_apply_settings(void);
