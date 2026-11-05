/*
 * SecOS Kernel - Input core + evdev (/dev/input/eventN)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * See input.h. Devices report events from interrupt context (PS/2) or from the
 * polled USB HID path; each open of /dev/input/eventN is a "client" with its
 * own ring of struct input_event. One irqsave spinlock guards devices, clients
 * and rings. Clients live in a static pool (no heap in the event path), each
 * with an embedded vfs_inode_t that the open() substitutes into the fd, the
 * same trick kernel/pty.c uses for pseudo-terminals.
 */
#include "input.h"
#include "vfs.h"
#include "spinlock.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "cap.h"
#include "kclock.h"
#include "debugcon.h"
#include "../mm/user_copy.h"
#include "../mm/elf_manifest.h"
#include <stddef.h>

#define INPUT_MAX_DEV    8
#define INPUT_MAX_CLIENT 16
#define EVDEV_RING       256          /* events per open; power of two */

#define E_PERM    (-1)
#define E_AGAIN   (-11)
#define E_FAULT   (-14)
#define E_BUSY    (-16)
#define E_NODEV   (-19)
#define E_INVAL   (-22)
#define E_ACCES   (-13)
#define E_MFILE   (-24)
#define E_NOTTY   (-25)
#define WOULDBLOCK (-2)

typedef struct evdev_client {
    vfs_inode_t ino;                  /* substituted into the fd at open() */
    input_dev_t* dev;
    int      used;
    int      refs;                    /* fd entries pointing at this client */
    int      clock_mono;              /* EVIOCSCLOCKID(CLOCK_MONOTONIC) */
    uint32_t head, tail;              /* ring indices (head = next write) */
    struct k_input_event ring[EVDEV_RING];
} evdev_client_t;

static spinlock_t g_input_lock = SPINLOCK_INIT;
static input_dev_t* g_dev[INPUT_MAX_DEV];
static int g_ndev;
static vfs_inode_t g_dev_ino[INPUT_MAX_DEV];   /* base nodes /input/eventN */
static vfs_inode_t g_dir_ino;                  /* /input */
static evdev_client_t g_client[INPUT_MAX_CLIENT];

static void scpy(char* d, const char* s, int max) {
    int i = 0;
    if (s) for (; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

/* ---- capability helpers ----------------------------------------------------- */
static void set_bit64(uint64_t* map, unsigned bit) { map[bit / 64] |= (1ull << (bit % 64)); }
static int  test_bit64(const uint64_t* map, unsigned bit) { return (map[bit / 64] >> (bit % 64)) & 1; }

void input_set_capability(input_dev_t* d, uint16_t type, uint16_t code) {
    if (!d || type >= EV_CNT) return;
    d->evbit |= (1u << type) | (1u << EV_SYN);
    switch (type) {
    case EV_KEY: if (code < KC_CNT) set_bit64(d->keybit, code); break;
    case EV_REL: if (code < REL_CNT) d->relbit |= (1u << code); break;
    case EV_ABS: if (code < ABS_CNT) d->absbit |= (1ull << code); break;
    default: break;
    }
}

void input_set_abs_params(input_dev_t* d, uint16_t axis, int32_t min, int32_t max) {
    if (!d || axis > ABS_Y) return;
    input_set_capability(d, EV_ABS, axis);
    d->absinfo[axis].minimum = min;
    d->absinfo[axis].maximum = max;
    d->absinfo[axis].value = min;
}

int input_register(input_dev_t* d) {
    if (!d) return -1;
    uint64_t fl = spin_lock_irqsave(&g_input_lock);
    if (g_ndev >= INPUT_MAX_DEV) { spin_unlock_irqrestore(&g_input_lock, fl); return -1; }
    int idx = g_ndev++;
    d->index = idx; d->pending = 0; d->grab = NULL;
    for (int i = 0; i < KC_CNT / 64; i++) d->keystate[i] = 0;
    d->evbit |= (1u << EV_SYN);
    g_dev[idx] = d;
    vfs_inode_t* n = &g_dev_ino[idx];
    scpy(n->path, "/input/event", sizeof(n->path));
    { int k = slen(n->path); n->path[k++] = (char)('0' + idx); n->path[k] = 0; }
    n->type = VFS_NODE_FILE; n->size = 0; n->fs_data = d;
    n->mode = 0020600; n->nlink = 1;                 /* S_IFCHR | 0600 */
    spin_unlock_irqrestore(&g_input_lock, fl);
    debugcon_writestring("[INPUT] /dev/input/event");
    debugcon_putchar((char)('0' + idx));
    debugcon_writestring(": "); debugcon_writestring(d->name); debugcon_writestring("\n");
    return idx;
}

int input_count(void) { return g_ndev; }

void input_poll_devices(void) {
    /* The polled backends (USB HID) are never driven from an ISR; this runs in
     * the reader's syscall context, outside the input lock (a poll reports
     * events, which takes it). */
    void (*seen[INPUT_MAX_DEV])(void); int ns = 0;
    for (int i = 0; i < g_ndev; i++) {
        void (*fn)(void) = g_dev[i] ? g_dev[i]->poll : NULL;
        if (!fn) continue;
        int dup = 0;
        for (int k = 0; k < ns; k++) if (seen[k] == fn) dup = 1;
        if (dup) continue;
        seen[ns++] = fn;
        fn();
    }
}

int input_grabbed(const input_dev_t* d) { return d && d->grab != NULL; }

/* ---- event delivery ------------------------------------------------------------ */
static void client_push(evdev_client_t* c, const struct k_input_event* ev) {
    uint32_t next = (c->head + 1) & (EVDEV_RING - 1);
    if (next == c->tail) {
        /* Overflow: drop everything queued and leave one SYN_DROPPED, as Linux
         * evdev does. The reader resynchronises on the next SYN_REPORT. */
        c->tail = c->head;
        struct k_input_event* d = &c->ring[c->head];
        *d = *ev;
        d->type = EV_SYN; d->code = SYN_DROPPED; d->value = 0;
        c->head = (c->head + 1) & (EVDEV_RING - 1);
        next = (c->head + 1) & (EVDEV_RING - 1);
        if (ev->type == EV_SYN && ev->code == SYN_REPORT) return;   /* end of the dropped packet */
    }
    c->ring[c->head] = *ev;
    c->head = next;
}

static void deliver_nolock(input_dev_t* d, uint16_t type, uint16_t code, int32_t value) {
    struct k_input_event ev;
    uint64_t rt = 0, mo = 0;
    int have_time = 0;
    ev.type = type; ev.code = code; ev.value = value;
    for (int i = 0; i < INPUT_MAX_CLIENT; i++) {
        evdev_client_t* c = &g_client[i];
        if (!c->used || c->dev != d) continue;
        if (d->grab && d->grab != c) continue;
        /* Read the clocks only when someone listens (an IRQ during early boot,
         * before kclock_init, then never touches them). */
        if (!have_time) { mo = kclock_mono_ns(); rt = kclock_real_ns(); have_time = 1; }
        uint64_t ns = c->clock_mono ? mo : rt;
        ev.sec = (int64_t)(ns / 1000000000ull);
        ev.usec = (int64_t)((ns % 1000000000ull) / 1000ull);
        client_push(c, &ev);
    }
}

void input_event(input_dev_t* d, uint16_t type, uint16_t code, int32_t value) {
    if (!d || d->index < 0) return;
    uint64_t fl = spin_lock_irqsave(&g_input_lock);
    if (type == EV_KEY) {
        if (code >= KC_CNT) goto out;
        int down = test_bit64(d->keystate, code);
        if (value) {
            if (down) value = 2;                    /* autorepeat */
            else set_bit64(d->keystate, code);
        } else {
            if (!down) goto out;                    /* release of a key never pressed */
            d->keystate[code / 64] &= ~(1ull << (code % 64));
        }
    } else if (type == EV_REL) {
        if (value == 0) goto out;
    } else if (type == EV_ABS) {
        if (code <= ABS_Y) {
            /* Like Linux: an absolute axis reports changes only (a tablet sends
             * its whole state in every report). */
            if (d->absinfo[code].value == value) goto out;
            d->absinfo[code].value = value;
        }
    }
    d->pending++;
    deliver_nolock(d, type, code, value);
out:
    spin_unlock_irqrestore(&g_input_lock, fl);
}

void input_sync(input_dev_t* d) {
    if (!d || d->index < 0) return;
    uint64_t fl = spin_lock_irqsave(&g_input_lock);
    if (d->pending) {
        d->pending = 0;
        deliver_nolock(d, EV_SYN, SYN_REPORT, 0);
    }
    spin_unlock_irqrestore(&g_input_lock, fl);
}

/* ---- devfs nodes ------------------------------------------------------------------ */
struct vfs_inode* input_devfs_lookup(const char* rel) {
    /* rel: "/input" or "/input/eventN" */
    const char* p = "/input";
    int i = 0;
    for (; p[i]; i++) if (rel[i] != p[i]) return NULL;
    if (rel[i] == 0) {
        vfs_inode_t* d = &g_dir_ino;
        scpy(d->path, "/input", sizeof(d->path));
        d->type = VFS_NODE_DIR; d->size = 0; d->fs_data = NULL; d->mode = 0040755; d->nlink = 2;
        return d;
    }
    const char* q = "/event";
    for (int k = 0; q[k]; k++, i++) if (rel[i] != q[k]) return NULL;
    if (rel[i] < '0' || rel[i] > '9' || rel[i + 1] != 0) return NULL;
    int idx = rel[i] - '0';
    if (idx >= g_ndev) return NULL;
    return &g_dev_ino[idx];
}

int input_devfs_readdir(const char* dir, input_iter_cb cb, void* user) {
    if (!cb) return -1;
    if (!dir) return -1;
    if (dir[0] == '/' && dir[1] == 0) {             /* the /dev root lists "input" */
        if (g_ndev) cb(input_devfs_lookup("/input"), user);
        return 0;
    }
    for (int i = 0; i < g_ndev; i++) cb(&g_dev_ino[i], user);
    return 0;
}

static int base_index(const vfs_inode_t* ino) {
    for (int i = 0; i < g_ndev; i++) if (ino == &g_dev_ino[i]) return i;
    return -1;
}

static evdev_client_t* client_of(const void* ino) {
    const uint8_t* p = (const uint8_t*)ino;
    const uint8_t* lo = (const uint8_t*)&g_client[0];
    const uint8_t* hi = (const uint8_t*)&g_client[INPUT_MAX_CLIENT];
    if (p < lo || p >= hi) return NULL;
    evdev_client_t* c = &g_client[(p - lo) / sizeof(evdev_client_t)];
    return (ino == &c->ino && c->used) ? c : NULL;
}
int input_is_client(const void* ino) { return client_of(ino) != NULL; }

/* Per-open client inodes never reach a filesystem's read/write (kernel/syscall.c
 * routes them to input_read/input_ioctl first); these stubs keep any generic
 * path that calls ops->read on them harmless. */
static int cl_read(vfs_inode_t* i, size_t o, void* b, size_t l) { (void)i; (void)o; (void)b; (void)l; return -1; }
static int cl_write(vfs_inode_t* i, size_t o, const void* b, size_t l) { (void)i; (void)o; (void)b; (void)l; return -1; }
static vfs_fs_ops_t g_client_ops = { .flags = VFS_FS_NOCACHE, .read = cl_read, .write = cl_write };

int input_open_hook(struct process* cp, struct vfs_inode** ino, int flags) {
    (void)flags;
    process_t* c = (process_t*)cp;
    if (!ino || !*ino) return 0;
    int idx = base_index(*ino);
    if (idx < 0) return 0;                          /* not an event node */
    /* Reading input devices exposes every keystroke: only a program whose signed
     * manifest declares CAP_INPUT may open them. */
    if (!cap_check_explicit(c, CAP_INPUT, 4 /*SYS_OPEN*/, "input-open")) return E_ACCES;
    uint64_t fl = spin_lock_irqsave(&g_input_lock);
    evdev_client_t* cl = NULL;
    for (int i = 0; i < INPUT_MAX_CLIENT; i++) if (!g_client[i].used) { cl = &g_client[i]; break; }
    if (!cl) { spin_unlock_irqrestore(&g_input_lock, fl); return E_MFILE; }
    cl->used = 1; cl->refs = 1; cl->clock_mono = 0;
    cl->head = cl->tail = 0;
    cl->dev = g_dev[idx];
    cl->ino = **ino;                                /* path, mode, ... of /input/eventN */
    cl->ino.fs_data = cl;
    cl->ino.ops = &g_client_ops;
    spin_unlock_irqrestore(&g_input_lock, fl);
    *ino = &cl->ino;
    return 0;
}

void input_obj_ref(void* ino) {
    uint64_t fl = spin_lock_irqsave(&g_input_lock);
    evdev_client_t* c = client_of(ino);
    if (c) c->refs++;
    spin_unlock_irqrestore(&g_input_lock, fl);
}

void input_obj_unref(void* ino) {
    uint64_t fl = spin_lock_irqsave(&g_input_lock);
    evdev_client_t* c = client_of(ino);
    if (c && --c->refs <= 0) {
        if (c->dev && c->dev->grab == c) c->dev->grab = NULL;   /* drop EVIOCGRAB */
        c->used = 0; c->dev = NULL; c->refs = 0;
    }
    spin_unlock_irqrestore(&g_input_lock, fl);
}

int input_read(void* ino, void* kbuf, int len, int nonblock) {
    evdev_client_t* c = client_of(ino);
    if (!c) return E_NODEV;
    if (len < (int)sizeof(struct k_input_event)) return E_INVAL;
    input_poll_devices();                           /* USB HID: fetch pending reports */
    int max = len / (int)sizeof(struct k_input_event);
    int n = 0;
    struct k_input_event* out = (struct k_input_event*)kbuf;
    uint64_t fl = spin_lock_irqsave(&g_input_lock);
    while (n < max && c->tail != c->head) {
        out[n++] = c->ring[c->tail];
        c->tail = (c->tail + 1) & (EVDEV_RING - 1);
    }
    spin_unlock_irqrestore(&g_input_lock, fl);
    if (n) return n * (int)sizeof(struct k_input_event);
    if (nonblock) return E_AGAIN;
    process_t* cur = sched_get_current();
    if (cur && signal_pending(cur)) return -4;      /* -EINTR */
    /* Block: re-poll in ~2 ms (the USB HID backend has no interrupt; a PS/2
     * event is picked up on the next re-run). kernel/syscall_trap.c sleeps on
     * sleep_until and re-issues the read. */
    extern uint64_t timer_get_ticks(void);
    if (cur) cur->sleep_until = timer_get_ticks() + 2;
    return WOULDBLOCK;
}

int input_poll(void* ino) {
    evdev_client_t* c = client_of(ino);
    if (!c) return 0x20;                            /* POLLNVAL */
    input_poll_devices();
    uint64_t fl = spin_lock_irqsave(&g_input_lock);
    int ready = (c->tail != c->head);
    spin_unlock_irqrestore(&g_input_lock, fl);
    return ready ? (0x001 | 0x040) : 0;             /* POLLIN | POLLRDNORM */
}

/* ---- ioctl ------------------------------------------------------------------------- */
/* Linux _IOC encoding: dir[31:30] size[29:16] type[15:8] nr[7:0]. */
#define IOC_NR(c)    ((unsigned)(c) & 0xFF)
#define IOC_TYPE(c)  (((unsigned)(c) >> 8) & 0xFF)
#define IOC_SIZE(c)  (((unsigned)(c) >> 16) & 0x3FFF)
#define IOC_DIR(c)   (((unsigned)(c) >> 30) & 0x3)
#define IOC_READ     2u
#define IOC_WRITE    1u

static int put_bytes(uint64_t arg, const void* src, unsigned have, unsigned want) {
    unsigned n = have < want ? have : want;
    if (n && copy_to_user((void*)arg, src, n)) return E_FAULT;
    /* Linux zero-fills nothing past `have` for strings, but bitmaps shorter than
     * the request leave the rest untouched; callers memset first. */
    return (int)n;
}

int input_ioctl(void* ino, uint64_t cmd, uint64_t arg) {
    evdev_client_t* c = client_of(ino);
    if (!c || !c->dev) return E_NODEV;
    input_dev_t* d = c->dev;
    if (IOC_TYPE(cmd) != 'E') return E_NOTTY;
    unsigned nr = IOC_NR(cmd), size = IOC_SIZE(cmd), dir = IOC_DIR(cmd);

    /* Fixed-size requests. */
    switch ((uint32_t)cmd) {
    case 0x80044501: {                              /* EVIOCGVERSION */
        int v = 0x010001;
        return copy_to_user((void*)arg, &v, sizeof(v)) ? E_FAULT : 0;
    }
    case 0x80084502:                                /* EVIOCGID */
        return copy_to_user((void*)arg, &d->id, sizeof(d->id)) ? E_FAULT : 0;
    case 0x80084503: {                              /* EVIOCGREP: delay, period (ms) */
        uint32_t rep[2] = { 250, 33 };
        return copy_to_user((void*)arg, rep, sizeof(rep)) ? E_FAULT : 0;
    }
    case 0x40084503:                                /* EVIOCSREP: accepted, ignored */
        return 0;
    case 0x40044590: {                              /* EVIOCGRAB (arg = int value) */
        uint64_t fl = spin_lock_irqsave(&g_input_lock);
        int r = 0;
        if (arg) {
            if (d->grab && d->grab != c) r = E_BUSY;
            else d->grab = c;
        } else {
            if (d->grab != c) r = E_INVAL;
            else d->grab = NULL;
        }
        spin_unlock_irqrestore(&g_input_lock, fl);
        return r;
    }
    case 0x400445a0: {                              /* EVIOCSCLOCKID */
        int id;
        if (copy_from_user(&id, (const void*)arg, sizeof(id))) return E_FAULT;
        if (id != KCLOCK_REALTIME && id != KCLOCK_MONOTONIC && id != KCLOCK_BOOTTIME) return E_INVAL;
        c->clock_mono = (id != KCLOCK_REALTIME);
        return 0;
    }
    default: break;
    }

    /* Variable-length read requests: EVIOCGNAME/PHYS/UNIQ/PROP/KEY/LED/SND/SW,
     * EVIOCGBIT(ev, len), EVIOCGABS(abs). */
    if (dir == IOC_READ) {
        if (nr == 0x06) return put_bytes(arg, d->name, (unsigned)slen(d->name) + 1, size);
        if (nr == 0x07) return put_bytes(arg, d->phys, (unsigned)slen(d->phys) + 1, size);
        if (nr == 0x08) return put_bytes(arg, "", 1, size);                 /* EVIOCGUNIQ */
        if (nr == 0x09) return put_bytes(arg, &d->propbit, sizeof(d->propbit), size);
        if (nr == 0x18) {                                                   /* EVIOCGKEY */
            uint64_t ks[KC_CNT / 64];
            uint64_t fl = spin_lock_irqsave(&g_input_lock);
            for (int i = 0; i < KC_CNT / 64; i++) ks[i] = d->keystate[i];
            spin_unlock_irqrestore(&g_input_lock, fl);
            return put_bytes(arg, ks, sizeof(ks), size);
        }
        if (nr == 0x19 || nr == 0x1a || nr == 0x1b) {                       /* LED/SND/SW: none */
            uint8_t z[8] = { 0 };
            return put_bytes(arg, z, size < sizeof(z) ? size : sizeof(z), size);
        }
        if (nr >= 0x20 && nr < 0x20 + EV_CNT) {                             /* EVIOCGBIT */
            unsigned ev = nr - 0x20;
            uint8_t z[8] = { 0 };
            switch (ev) {
            case 0:      return put_bytes(arg, &d->evbit, sizeof(d->evbit), size);
            case EV_KEY: return put_bytes(arg, d->keybit, sizeof(d->keybit), size);
            case EV_REL: return put_bytes(arg, &d->relbit, sizeof(uint16_t), size);
            case EV_ABS: return put_bytes(arg, &d->absbit, sizeof(d->absbit), size);
            default:     return put_bytes(arg, z, size < sizeof(z) ? size : sizeof(z), size);
            }
        }
        if (nr >= 0x40 && nr < 0x40 + ABS_CNT) {                            /* EVIOCGABS */
            unsigned axis = nr - 0x40;
            struct k_input_absinfo ai = { 0, 0, 0, 0, 0, 0 };
            if (!(d->absbit & (1ull << axis))) return E_INVAL;
            if (axis <= ABS_Y) {
                uint64_t fl = spin_lock_irqsave(&g_input_lock);
                ai = d->absinfo[axis];
                spin_unlock_irqrestore(&g_input_lock, fl);
            }
            unsigned n = size < sizeof(ai) ? size : sizeof(ai);
            return copy_to_user((void*)arg, &ai, n) ? E_FAULT : 0;
        }
    }
    if (dir == IOC_WRITE && nr >= 0xc0 && nr < 0xc0 + ABS_CNT) return E_INVAL;  /* EVIOCSABS */
    return E_INVAL;
}
