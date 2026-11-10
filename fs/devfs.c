/*
 * devfs.c — [M23] /dev virtual filesystem.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Exposes devices as files under /dev so source-built (and signed) Linux-style
 * programs can open() them. Trust is rooted in the mandatory signature: only
 * signed code runs, so a signed program gets ambient access to /dev (the Driver
 * Space capability model is orthogonal — a /dev/<blk> read still goes through the
 * kernel block driver). Char devices (null/zero/full/random/urandom/console/tty)
 * are synthesized; block devices (sda/nvme0n1/usb0/vda...) are byte-addressed
 * over the registered block_dev_t. A pure namespace: no create/mkdir/remove.
 *
 * GUI (M40): /dev/fb0 is the framebuffer (drivers/fbdev.c) and
 * /dev/input/eventN the evdev input devices (drivers/input.c). Unlike the rest
 * of /dev they are NOT ambient: opening them needs CAP_INPUT / CAP_DISPLAY in
 * the opener's signed manifest (devfs_open_hook).
 *
 * Terminals: /dev/console is the console; /dev/tty is the opener's controlling
 * terminal (the console, or a pty slave - ksys_open asks kernel/pty.c to
 * substitute it); /dev/ptmx allocates a pseudo-terminal and /dev/pts/N are the
 * live slaves (both served by kernel/pty.c).
 */
#include "vfs.h"
#include "block.h"
#include "terminal.h"
#include "keyboard.h"
#include "pty.h"
#include "input.h"
#include "fbdev.h"
#include "cap.h"
#include "process.h"
#include "cred.h"
#include "heap.h"
#include "user_copy.h"
#include "spinlock.h"
#include <stddef.h>
#include <stdint.h>

extern uint64_t timer_get_ticks(void);

enum dev_kind { DEV_NULL, DEV_ZERO, DEV_FULL, DEV_RANDOM, DEV_CONSOLE, DEV_BLOCK,
                DEV_TTY /* controlling terminal */, DEV_PTMX /* pty multiplexer */,
                DEV_KMSG /* kernel log */, DEV_FB /* [M40] framebuffer */,
                DEV_RTC /* [U10] CMOS clock: hwclock's RTC_RD_TIME/RTC_SET_TIME */ };

struct dev_node {
    char name[16];
    enum dev_kind kind;
    block_dev_t* blk;     // for DEV_BLOCK
};

/*
 * [M51 C2] The node table is built when devfs is mounted (all boot-time
 * drivers are up by then) instead of lazily on the first access, which could
 * run on two CPUs at once. Node i and its inode are filled completely before
 * g_nnodes is raised past it (release store; readers load it with acquire), so
 * lookups and listings read the table without a lock. Nothing in a published
 * node or its inode changes afterwards except mode/uid/gid (chmod/chown, e.g.
 * login chowning /dev/console), which devfs_setattr writes under g_devfs_lock.
 * Block devices registered after the mount (shell `usbrescan`) are appended by
 * devfs_sync_blocks under the same lock -- the old lazy build never showed
 * them. The lock is a leaf-ish spinlock (S15): inside it only the block
 * registry (S64) is taken, and devfs never calls the pty/input namespaces
 * (S10/S15) while holding it.
 */
#define DEVFS_MAX 24
static struct dev_node g_nodes[DEVFS_MAX];
static volatile int g_nnodes;
static vfs_inode_t g_inodes[DEVFS_MAX + 1];   // +1 for the root dir
static int g_blk_seen;                        // block_count() the table reflects
static spinlock_t g_devfs_lock = SPINLOCK_INIT_CLASS(LK_S15_SUBSYS, "devfs");
static vfs_fs_ops_t devfs_ops;

static int nnodes(void) { return __atomic_load_n(&g_nnodes, __ATOMIC_ACQUIRE); }

static void sset(char* d, const char* s) { int i=0; while(s[i] && i<15){ d[i]=s[i]; i++; } d[i]=0; }
static int seq(const char* a, const char* b){ int i=0; while(a[i]&&b[i]){ if(a[i]!=b[i]) return 0; i++; } return a[i]==b[i]; }

// [M43] Device permissions (Linux defaults): the data sinks/sources and the
// terminal multiplexers are world read-write; the console belongs to root
// until login gives it to the user who logged in there; the kernel log and
// block devices are root's (group disk: nobody is in it by default); the
// framebuffer is root:video and the input devices root:input -- and those two
// additionally need CAP_DISPLAY / CAP_INPUT in the opener's signed manifest.
static uint32_t dev_mode(enum dev_kind k, uint32_t* gid) {
    *gid = GID_ROOT;
    switch (k) {
    case DEV_NULL: case DEV_ZERO: case DEV_FULL: case DEV_RANDOM:
    case DEV_TTY: case DEV_PTMX:   return 0020666;
    case DEV_CONSOLE:              return 0020600;
    case DEV_KMSG:                 return 0020600;
    case DEV_FB:     *gid = GID_VIDEO; return 0020660;
    case DEV_RTC:                  return 0020600;    /* setting the clock: root */
    case DEV_BLOCK:  *gid = GID_DISK;  return 0060660;
    }
    return 0020600;
}
static uint64_t dev_size(const struct dev_node* d) {
    if (d->kind == DEV_BLOCK && d->blk) return d->blk->sector_count * (uint64_t)d->blk->sector_size;
    return 0;   // char devices report size 0
}

// Append one node with its complete inode, then publish it. Caller holds
// g_devfs_lock (or is the mount, before devfs is reachable).
static void add(const char* name, enum dev_kind k, block_dev_t* b) {
    int i = g_nnodes;
    if (i >= DEVFS_MAX) return;
    sset(g_nodes[i].name, name);
    g_nodes[i].kind = k;
    g_nodes[i].blk = b;
    vfs_inode_t* ino = &g_inodes[i];
    ino->path[0]='/'; int p=1;
    for (int j=0; g_nodes[i].name[j] && p<255; j++) ino->path[p++]=g_nodes[i].name[j];
    ino->path[p]=0;
    ino->type = VFS_NODE_FILE;
    ino->size = dev_size(&g_nodes[i]);
    ino->fs_data = &g_nodes[i];
    ino->ops = &devfs_ops;
    uint32_t gid;
    ino->mode = dev_mode(k, &gid);   // kept across lookups; chmod/chown change it
    ino->uid = 0;
    ino->gid = gid;
    ino->nlink = 1;
    __atomic_store_n(&g_nnodes, i + 1, __ATOMIC_RELEASE);
}
static int has_blk(const block_dev_t* b) {
    for (int i=0;i<g_nnodes;i++) if (g_nodes[i].kind == DEV_BLOCK && g_nodes[i].blk == b) return 1;
    return 0;
}
// Append block devices registered since the table was last synced (the
// registry only grows). Cheap when nothing changed: one block_count().
static void devfs_sync_blocks(void) {
    int n = block_count();
    if (n == __atomic_load_n(&g_blk_seen, __ATOMIC_ACQUIRE)) return;
    uint64_t fl = spin_lock_irqsave(&g_devfs_lock);
    n = block_count();
    for (int i = 0; i < n; i++) {
        block_dev_t* b = block_get(i);
        if (b && b->name && !has_blk(b)) add(b->name, DEV_BLOCK, b);
    }
    __atomic_store_n(&g_blk_seen, n, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_devfs_lock, fl);
}

// Build the device table from the synthetic char devices + every registered
// block device. Called once, from devfs_mount.
static void devfs_build(void) {
    uint64_t fl = spin_lock_irqsave(&g_devfs_lock);
    if (g_nnodes) { spin_unlock_irqrestore(&g_devfs_lock, fl); return; }
    vfs_inode_t* root = &g_inodes[DEVFS_MAX];
    root->path[0]='/'; root->path[1]=0; root->type=VFS_NODE_DIR; root->size=0; root->fs_data=NULL;
    root->ops = &devfs_ops;
    root->mode = 0040755; root->uid = root->gid = 0; root->nlink = 2;   // [M43] /dev: root 0755
    add("null", DEV_NULL, NULL);
    add("zero", DEV_ZERO, NULL);
    add("full", DEV_FULL, NULL);
    add("random", DEV_RANDOM, NULL);
    add("urandom", DEV_RANDOM, NULL);
    add("console", DEV_CONSOLE, NULL);
    add("tty", DEV_TTY, NULL);
    add("ptmx", DEV_PTMX, NULL);
    add("kmsg", DEV_KMSG, NULL);                 // kernel log, one record per read()
    // [M40] linear framebuffer. Always in the table (the table may be built
    // before the boot path set the framebuffer up, e.g. under UEFI); lookups
    // and listings hide it while fbdev_present() is false.
    add("fb0", DEV_FB, NULL);
    add("rtc0", DEV_RTC, NULL);                  // [U10] (ioctl only: read/write fail)
    int n = block_count();
    for (int i = 0; i < n; i++) {
        block_dev_t* b = block_get(i);
        if (b && b->name) add(b->name, DEV_BLOCK, b);
    }
    __atomic_store_n(&g_blk_seen, n, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_devfs_lock, fl);
}

// The inode of node index i (or the root when i<0): fully built at add().
static vfs_inode_t* node_inode(int i) { return &g_inodes[i < 0 ? DEVFS_MAX : i]; }

static int find_node(const char* rel) {
    // rel is "/name"; skip the leading slash.
    if (!rel || rel[0] != '/') return -2;
    if (rel[1] == 0) return -1;             // root dir
    const char* name = rel + 1;
    int n = nnodes();
    for (int i=0;i<n;i++) {
        if (!seq(g_nodes[i].name, name)) continue;
        if (g_nodes[i].kind == DEV_FB && !fbdev_present()) return -2;
        return i;
    }
    return -2;                              // not found
}

static vfs_inode_t* devfs_lookup(const char* rel) {
    vfs_inode_t* pt = pty_devfs_lookup(rel);           /* /pts, /pts/N */
    if (pt) return pt;
    vfs_inode_t* in = input_devfs_lookup(rel);         /* /input, /input/eventN */
    if (in) return in;
    devfs_sync_blocks();
    int i = find_node(rel);
    if (i == -2) return NULL;
    return node_inode(i);
}

// The callbacks run with no devfs lock held (published nodes never change).
static int devfs_readdir(const char* dir, vfs_iter_cb cb, void* user) {
    if (!cb) return -1;
    if (dir && seq(dir, "/pts")) return pty_devfs_readdir(cb, user);
    if (dir && seq(dir, "/input")) return input_devfs_readdir(dir, (input_iter_cb)cb, user);
    if (!(dir && dir[0]=='/' && dir[1]==0)) return -1;   // only the root dir, /pts and /input list
    devfs_sync_blocks();
    int n = nnodes();
    for (int i=0;i<n;i++) {
        if (g_nodes[i].kind == DEV_FB && !fbdev_present()) continue;
        cb(node_inode(i), user);
    }
    cb(pty_devfs_lookup("/pts"), user);
    input_devfs_readdir("/", (input_iter_cb)cb, user);
    return 0;
}

// [M51 C2] chmod/chown/utimes on a device node: devfs keeps the metadata in
// the node's inode (as the VFS fallback did), written under g_devfs_lock. The
// lookup runs before the lock: /pts/N and /input/eventN inodes belong to
// kernel/pty.c and drivers/input.c, whose own locks rank below this one.
static int devfs_setattr(const char* rel, const vfs_attr_t* a, unsigned valid) {
    if (!a) return -1;
    int pr = pty_devfs_setattr(rel, a, valid);                    /* [M52] /pts/N: pty lock */
    if (pr <= 0) return pr;
    vfs_inode_t* ino = devfs_lookup(rel);
    if (!ino) return -2;                                           // -ENOENT
    uint64_t fl = spin_lock_irqsave(&g_devfs_lock);
    if (valid & VFS_ATTR_MODE) {
        uint32_t t = ino->mode & 0170000u;
        if (!t) t = ino->type==VFS_NODE_DIR ? 0040000u : ino->type==VFS_NODE_SYMLINK ? 0120000u : 0100000u;
        ino->mode = t | (a->mode & 07777u);
    }
    if (valid & VFS_ATTR_UID)   ino->uid = a->uid;
    if (valid & VFS_ATTR_GID)   ino->gid = a->gid;
    if (valid & VFS_ATTR_ATIME) ino->atime = a->atime;
    if (valid & VFS_ATTR_MTIME) ino->mtime = a->mtime;
    spin_unlock_irqrestore(&g_devfs_lock, fl);
    return 0;
}

// /dev/random and /dev/urandom: the kernel CSPRNG (crypto/csprng.c, SHA-256
// DRBG seeded from RDSEED/RDRAND + TSC jitter). Never blocks.
#include "../crypto/csprng.h"

static int blk_byte_io(block_dev_t* b, size_t off, void* buf, size_t len, int write);

/* Is this inode /dev/tty or /dev/console? (termios ioctls apply to it.) */
int devfs_is_console(void* p) {
    vfs_inode_t* inode = (vfs_inode_t*)p;
    if (!inode || !inode->fs_data) return 0;
    struct dev_node* d = (struct dev_node*)inode->fs_data;
    return (d >= g_nodes && d < g_nodes + DEVFS_MAX && (d->kind == DEV_CONSOLE || d->kind == DEV_TTY));
}
static int devfs_kind_is(void* p, enum dev_kind k) {
    vfs_inode_t* inode = (vfs_inode_t*)p;
    if (!inode || !inode->fs_data) return 0;
    struct dev_node* d = (struct dev_node*)inode->fs_data;
    return (d >= g_nodes && d < g_nodes + DEVFS_MAX && d->kind == k);
}
/* /dev/tty (resolved per opener) and /dev/ptmx (a new pty per open). */
int devfs_is_devtty(void* p) { return devfs_kind_is(p, DEV_TTY); }
int devfs_is_ptmx(void* p) { return devfs_kind_is(p, DEV_PTMX); }
int devfs_is_fb(void* p) { return devfs_kind_is(p, DEV_FB); }
int devfs_is_rtc(void* p) { return devfs_kind_is(p, DEV_RTC); }

/* [U10] /dev/rtc0 ioctls (Linux <linux/rtc.h>): struct rtc_time is nine ints
 * laid out like the first nine of struct tm; the CMOS clock holds UTC. */
#include "../drivers/rtc.h"
struct lx_rtc_time { int sec, min, hour, mday, mon, year, wday, yday, isdst; };
#define RTC_RD_TIME_  0x80247009u
#define RTC_SET_TIME_ 0x4024700au
int devfs_rtc_ioctl(void* cp, uint64_t cmd, uint64_t arg, int writable) {
    process_t* c = (process_t*)cp;
    struct lx_rtc_time t;
    struct rtc_datetime d;
    if (cmd == RTC_RD_TIME_) {
        if (!rtc_read(&d)) return -5;                          /* EIO */
        t.sec = d.second; t.min = d.minute; t.hour = d.hour; t.mday = d.day;
        t.mon = d.month - 1; t.year = d.year - 1900; t.wday = 0; t.yday = 0; t.isdst = 0;
        /* day of the week / year (proleptic Gregorian) for tm consumers */
        static const int cum[12] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
        int y = d.year, leap = (y % 4 == 0 && y % 100) || y % 400 == 0;
        t.yday = cum[t.mon] + t.mday - 1 + (leap && t.mon > 1);
        long days = 365L * (y - 1970) + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400 + t.yday;
        t.wday = (int)((days + 4) % 7);
        return copy_to_user((void*)arg, &t, sizeof(t)) ? -14 : 0;
    }
    if (cmd == RTC_SET_TIME_) {
        if (!writable) return -9;                              /* EBADF: opened read-only */
        if (!cred_is_root(c) || !cap_check(c, CAP_TIME, 53, "rtc-set")) return -13;
        if (copy_from_user(&t, (const void*)arg, sizeof(t))) return -14;
        if (t.year < 100 || t.year > 199 || t.mon < 0 || t.mon > 11) return -22;
        d.second = (uint8_t)t.sec; d.minute = (uint8_t)t.min; d.hour = (uint8_t)t.hour;
        d.day = (uint8_t)t.mday; d.month = (uint8_t)(t.mon + 1); d.year = (uint16_t)(t.year + 1900);
        return rtc_write(&d) ? 0 : -22;
    }
    return -25;                                                /* ENOTTY */
}

/* [M40] open() of a GUI device: /dev/fb0 needs CAP_DISPLAY, /dev/input/eventN
 * CAP_INPUT (and gets a per-open evdev client). 0 = allowed / not a GUI node,
 * <0 = -errno. */
int devfs_open_hook(void* cp, vfs_inode_t** ino, int flags) {
    process_t* c = (process_t*)cp;
    if (!ino || !*ino) return 0;
    if (devfs_is_fb(*ino))
        return cap_check_explicit(c, CAP_DISPLAY, 4 /*SYS_OPEN*/, "fb0-open") ? 0 : -13;
    return input_open_hook(c, ino, flags);
}

static int devfs_read(vfs_inode_t* inode, size_t off, void* buf, size_t len) {
    if (!inode || !inode->fs_data) return -1;
    struct dev_node* d = (struct dev_node*)inode->fs_data;
    uint8_t* out = (uint8_t*)buf;
    switch (d->kind) {
        case DEV_NULL: return 0;                                   // EOF
        case DEV_ZERO: for (size_t i=0;i<len;i++) out[i]=0; return (int)len;
        case DEV_FULL: for (size_t i=0;i<len;i++) out[i]=0; return (int)len;
        case DEV_RANDOM: {
            csprng_get(out, len);
            return (int)len;
        }
        case DEV_PTMX: return -1;                                  // replaced at open
        case DEV_TTY:
        case DEV_CONSOLE: {                                        // [M25] console TTY read
            // -2 = would block: SYS_READ blocks the caller (wait_tty) and re-runs.
            extern int tty_read_try(void* buf, int len);
            return tty_read_try(buf, (int)(len > 4096 ? 4096 : len));
        }
        case DEV_BLOCK: return blk_byte_io(d->blk, off, buf, len, 0);
        case DEV_FB: return fbdev_read(off, buf, len);
        case DEV_KMSG: {
            extern int klog_kmsg_read(uint64_t off, char* out, size_t len);
            return klog_kmsg_read(off, (char*)buf, len);
        }
    }
    return -1;
}

static int devfs_write(vfs_inode_t* inode, size_t off, const void* buf, size_t len) {
    if (!inode || !inode->fs_data) return -1;
    struct dev_node* d = (struct dev_node*)inode->fs_data;
    const uint8_t* in = (const uint8_t*)buf;
    switch (d->kind) {
        case DEV_NULL: case DEV_ZERO: return (int)len;            // sink
        case DEV_FULL: return -1;                                 // ENOSPC
        case DEV_RANDOM: return (int)len;                         // reseed ignored
        case DEV_PTMX: return -1;                                 // replaced at open
        case DEV_TTY:
        case DEV_CONSOLE: { extern int tty_write(const void*, int); return tty_write(in, (int)len); }
        case DEV_BLOCK: return blk_byte_io(d->blk, off, (void*)(uintptr_t)buf, len, 1);
        case DEV_FB: return fbdev_write(off, buf, len);
        case DEV_KMSG: {                                          // userspace -> kernel log
            extern void klog_putc(char c);
            for (size_t i=0;i<len && i<512;i++) klog_putc((char)in[i]);
            if (len && in[len-1] != '\n') klog_putc('\n');
            return (int)len;
        }
    }
    return -1;
}

// Byte-addressed I/O over a sector device: whole sectors straight to/from the
// caller's (kernel) buffer, a one-sector bounce for the unaligned head/tail.
// [M50 B3] Goes through the block layer and holds the base disk's lock for the
// whole request, so a partial-sector read-modify-write is atomic against every
// other user of the disk (and of its other partitions). The bounce is per call:
// two CPUs can be in here at once for different disks.
#define BLK_BYTE_IO_MAX_SECTORS 128
static int blk_byte_io(block_dev_t* b, size_t off, void* buf, size_t len, int write) {
    if (!b) return -1;
    if (write && !b->write) return -1;
    uint32_t ss = b->sector_size;
    uint64_t total = b->sector_count * (uint64_t)ss;
    if (ss == 0 || ss > 4096) return -1;
    if (off >= total) return 0;
    if (off + len > total) len = (size_t)(total - off);
    uint8_t* sec = 0;
    if ((off % ss) || (len % ss)) {
        sec = (uint8_t*)kmalloc(ss);
        if (!sec) return -1;
    }
    uint8_t* p = (uint8_t*)buf;
    size_t done = 0;
    int err = 0;
    block_lock(b);
    while (done < len) {
        uint64_t abs = off + done;
        uint64_t lba = abs / ss;
        uint32_t soff = (uint32_t)(abs % ss);
        if (soff == 0 && len - done >= ss) {
            // Whole-sector fast path: as many sectors as are left (capped).
            uint64_t n = (len - done) / ss;
            if (n > BLK_BYTE_IO_MAX_SECTORS) n = BLK_BYTE_IO_MAX_SECTORS;
            int r = write ? block_write_locked(b, lba, p + done, (uint32_t)n)
                          : block_read_locked(b, lba, p + done, (uint32_t)n);
            if (r != (int)n) { err = 1; break; }
            done += (size_t)n * ss;
            continue;
        }
        uint32_t chunk = ss - soff; if (chunk > len - done) chunk = (uint32_t)(len - done);
        if (block_read_locked(b, lba, sec, 1) != 1) { err = 1; break; }
        if (write) {
            for (uint32_t i=0;i<chunk;i++) sec[soff+i] = p[done+i];
            if (block_write_locked(b, lba, sec, 1) != 1) { err = 1; break; }
        } else {
            for (uint32_t i=0;i<chunk;i++) p[done+i] = sec[soff+i];
        }
        done += chunk;
    }
    block_unlock(b);
    if (sec) kfree(sec);
    return err ? -1 : (int)done;
}

static int devfs_ro_create(const char* p, const void* d, size_t s){ (void)p;(void)d;(void)s; return -1; }
static int devfs_ro_mkdir(const char* p){ (void)p; return -1; }
static int devfs_ro_remove(const char* p){ (void)p; return -1; }
static int devfs_ro_rename(const char* a, const char* b){ (void)a;(void)b; return -1; }
static int devfs_ro_trunc(const char* p, size_t s){ (void)p;(void)s; return -1; }

static vfs_fs_ops_t devfs_ops = {
    .flags = VFS_FS_NOCACHE,
    .lookup = devfs_lookup,
    .readdir = devfs_readdir,
    .read = devfs_read,
    .write = devfs_write,
    .create = devfs_ro_create,
    .mkdir = devfs_ro_mkdir,
    .remove = devfs_ro_remove,
    .rename = devfs_ro_rename,
    .truncate = devfs_ro_trunc,
    .setattr = devfs_setattr,
};

int devfs_mount(const char* mount_point) {
    extern int vfs_mount(const char*, const vfs_fs_ops_t*, const char*);
    devfs_build();                    // [M51 C2] once, before /dev is reachable
    return vfs_mount(mount_point, &devfs_ops, "devfs");
}
