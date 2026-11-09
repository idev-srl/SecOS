/*
 * udrv_negtest.c — [M57] SYS_UDRV argument-validation table (Driver Space v2,
 * user/include/secos/udrv.h; error codes in docs/devlog/M57-udrv-edu.md).
 *
 * A list of rows "operation + argument -> expected result". Each row makes
 * one call and compares the raw kernel return value (0 or -errno) with the
 * row's expectation for the environment the gate (tools/udrv_gate.sh) boots:
 *
 *   --env translated  edu assigned, IOMMU translates it, udrv.noir: DMA rights
 *   --env unsafe      edu assigned, no IOMMU, udrv.unsafe: DMA rights (tainted)
 *   --env noir        edu assigned, IOMMU, no interrupt remapping: MMIO only
 *   --env nommu       edu assigned, no IOMMU: MMIO only
 *   --env nodev       no udrv= device at all
 *   --env user        the same table from udrv_user (plain, non-driver manifest)
 *
 * Table A (every environment) needs no claim. Table B runs on a claim of edu
 * (1234:11e8) in the four assigned environments; its rows are stateful and
 * run in order (a row may map the BAR or allocate DMA that later rows use).
 * Output: one "[udrv_negtest] A07 <name>: PASS|FAIL|SKIP rc=... want=..." line
 * per row, then "[udrv_negtest] RESULT rows=N pass=P fail=F skip=S" and
 * "[udrv_negtest] PASS|FAIL pid=N". Exit status 0 = every row passed.
 * SPDX-License-Identifier: MIT
 */
#include "libsecos.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <poll.h>
#include <secos/udrv.h>

#define EPERM_       1
#define EIO_         5
#define EBADF_       9
#define EAGAIN_     11
#define ENOMEM_     12
#define EACCES_     13
#define EFAULT_     14
#define EBUSY_      16
#define ENODEV_     19
#define EINVAL_     22
#define EOPNOTSUPP_ 95

#define SKIP        1000            /* expectation: row does not apply here */
#define SYS_READ_   3
#define SYS_WRITE_  2
#define SYS_FCNTL_  61
#define SYS_SETRESUID_ 106
#define F_SETFL_    4
#define O_NONBLOCK_ 0x800

#define EDU_IRQ_STAT  0x24
#define EDU_IRQ_RAISE 0x60
#define EDU_IRQ_ACK   0x64

/* Environment classes: the expectation column a row uses. */
enum { CL_DMA, CL_MMIO, CL_NODEV, CL_USER, CL_N };

/* What a row calls. */
enum {
    C_UDRV,         /* secos_udrv(op, handle, arg, size)                    */
    C_READ,         /* read(handle, arg, size) on the claim fd              */
    C_WRITE,        /* write(handle, arg, size)                             */
    C_MUNMAP,       /* munmap(arg, size)                                    */
    C_MPROTECT,     /* mprotect(arg, size, PROT_READ)                       */
    C_MMAP_FD,      /* mmap(NULL, size, PROT_READ, MAP_SHARED, claim fd, 0) */
    C_EUID,         /* the C_UDRV call with euid 65534 (real/saved uid 0)   */
    C_CHILD,        /* the C_UDRV/C_READ/C_WRITE call (v2) in a fork child  */
};

/* Handle selectors. */
enum { H_NONE, H_CLAIM, H_STDIN, H_FD31, H_FD32, H_NEG, H_HUGE };

/* Argument selectors. A_BUF: a zeroed struct filled from v0..v2. */
enum { A_BUF, A_NULL, A_LOW, A_KERNEL, A_NONCANON, A_TOP, A_RO, A_BAR, A_DMA, A_DMA_PLUS };

/* Size selectors (C_UDRV); other calls take the size literally. */
#define SZ_EXACT  (-1L)
#define SZ_MINUS1 (-2L)
#define SZ_PLUS1  (-3L)
#define SZ_HUGE   (-4L)

/* v0 value: the offset of edu's MSI capability (found at claim time). */
#define V_MSI_CAP 0xC0DE0001ull

typedef struct {
    const char* id;
    const char* name;
    int      call;
    long     op;
    int      h;
    int      a;
    long     size;
    uint64_t v0, v1, v2;         /* op fields, see fill() */
    int      exp[CL_N];          /* 0 / -errno / SKIP per environment class */
    void   (*pre)(void);         /* optional: state a row needs */
} row_t;

#define ALL(e)       { e, e, e, e }
#define DM(d, m)     { d, m, SKIP, SKIP }

static int g_cls, g_fd = -1;
static uint64_t g_bar, g_dma, g_msi_cap;
static int g_pass, g_fail, g_skip;
/* Read-only (.rodata), non-zero so it is not placed in .bss. */
static const uint64_t k_ro[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(b) - 1) n = (int)sizeof(b) - 1;
    write(1, b, (size_t)n);                   /* one write per line */
}

static const char* ename(long r) {
    switch (r) {
    case 0: return "0";
    case -EPERM_: return "-EPERM";
    case -EIO_: return "-EIO";
    case -EBADF_: return "-EBADF";
    case -EAGAIN_: return "-EAGAIN";
    case -ENOMEM_: return "-ENOMEM";
    case -EACCES_: return "-EACCES";
    case -EFAULT_: return "-EFAULT";
    case -EBUSY_: return "-EBUSY";
    case -ENODEV_: return "-ENODEV";
    case -EINVAL_: return "-EINVAL";
    case -EOPNOTSUPP_: return "-EOPNOTSUPP";
    default: return NULL;
    }
}

static volatile uint32_t* bar32(uint32_t off) { return (volatile uint32_t*)(uintptr_t)(g_bar + off); }

/* ------------------------------------------------------------ row state */
/* An interrupt raised and pending on the claim fd (DMA environments). */
static void pre_irq_pending(void) {
    if (g_cls != CL_DMA || !g_bar) return;
    struct udrv_ack a = { 1, 0 };
    secos_syscall(SYS_WRITE_, g_fd, (long)&a, sizeof(a), 0, 0);      /* unmask */
    *bar32(EDU_IRQ_RAISE) = 0x1;
    struct pollfd pf = { g_fd, POLLIN, 0 };
    poll(&pf, 1, 2000);
}

/* The interrupt of pre_irq_pending acknowledged on the device and the fd. */
static void pre_irq_ack(void) {
    if (g_cls != CL_DMA || !g_bar) return;
    *bar32(EDU_IRQ_ACK) = *bar32(EDU_IRQ_STAT);
    struct udrv_ack a = { 1, 0 };
    secos_syscall(SYS_WRITE_, g_fd, (long)&a, sizeof(a), 0, 0);
}

static void pre_nonblock(void) { secos_syscall(SYS_FCNTL_, g_fd, F_SETFL_, O_NONBLOCK_ | 2, 0, 0); }
static void pre_close(void) { close(g_fd); }

/* ------------------------------------------------------------ the table */
#define EDU_BYID  UDRV_CLAIM_BY_ID, 0x123411e8ull
#define BDF(b, d, f) 0, ((uint64_t)(b) << 16 | (uint64_t)(d) << 8 | (f))

/* Table A: no claim needed. Columns: DMA, MMIO, NODEV, USER. */
static const row_t g_a[] = {
    { "A01", "op 0", C_UDRV, 0, H_NONE, A_BUF, 0, 0, 0, 0, ALL(-EINVAL_), NULL },
    { "A02", "op 12 (past NET_ATTACH)", C_UDRV, 12, H_NONE, A_BUF, 0, 0, 0, 0, ALL(-EINVAL_), NULL },
    { "A03", "op 2^32+1 (upper bits set)", C_UDRV, 0x100000001L, H_NONE, A_BUF, SZ_EXACT, EDU_BYID, 0, ALL(-EINVAL_), NULL },
    { "A04", "op -1", C_UDRV, -1L, H_NONE, A_BUF, 0, 0, 0, 0, ALL(-EINVAL_), NULL },
    { "A05", "CLAIM size sizeof-1", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_MINUS1, EDU_BYID, 0, ALL(-EINVAL_), NULL },
    { "A06", "CLAIM size sizeof+1", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_PLUS1, EDU_BYID, 0, ALL(-EINVAL_), NULL },
    { "A07", "CLAIM size 0", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_BUF, 0, EDU_BYID, 0, ALL(-EINVAL_), NULL },
    { "A08", "CLAIM size ~0", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_HUGE, EDU_BYID, 0, ALL(-EINVAL_), NULL },
    { "A09", "CLAIM arg NULL", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_NULL, SZ_EXACT, 0, 0, 0, ALL(-EFAULT_), NULL },
    { "A10", "CLAIM arg 0x1000 (unmapped user page)", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_LOW, SZ_EXACT, 0, 0, 0, ALL(-EFAULT_), NULL },
    { "A11", "CLAIM arg kernel half", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_KERNEL, SZ_EXACT, 0, 0, 0, ALL(-EFAULT_), NULL },
    { "A12", "CLAIM arg non-canonical", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_NONCANON, SZ_EXACT, 0, 0, 0, ALL(-EFAULT_), NULL },
    { "A13", "CLAIM arg straddles the top of user space", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_TOP, SZ_EXACT, 0, 0, 0, ALL(-EFAULT_), NULL },
    { "A14", "CLAIM flags BY_ID|0x2 (unknown bit)", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_EXACT, 0x3, 0x123411e8ull, 0,
      { -EINVAL_, -EINVAL_, -ENODEV_, -EINVAL_ }, NULL },
    { "A15", "CLAIM flags BY_ID|0x80000000 (unknown bit)", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_EXACT, 0x80000001ull, 0x123411e8ull, 0,
      { -EINVAL_, -EINVAL_, -ENODEV_, -EINVAL_ }, NULL },
    { "A16", "CLAIM bdf 00:1f.0 (not in udrv=)", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_EXACT, BDF(0, 0x1f, 0), 0,
      { -ENODEV_, -ENODEV_, -ENODEV_, -EPERM_ }, NULL },
    { "A17", "CLAIM bdf 00:04.1 (no such function)", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_EXACT, BDF(0, 4, 1), 0,
      { -ENODEV_, -ENODEV_, -ENODEV_, -EPERM_ }, NULL },
    { "A18", "CLAIM id 1234:ffff (not assigned)", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_EXACT, UDRV_CLAIM_BY_ID, 0x1234ffffull, 0,
      { -ENODEV_, -ENODEV_, -ENODEV_, -EPERM_ }, NULL },
    { "A19", "CLAIM id 8086:10d3 (in manifest, not assigned)", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_EXACT, UDRV_CLAIM_BY_ID, 0x808610d3ull, 0,
      { -ENODEV_, -ENODEV_, -ENODEV_, -EPERM_ }, NULL },
    { "A20", "CLAIM edu with euid 65534", C_EUID, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_EXACT, EDU_BYID, 0,
      { -EPERM_, -EPERM_, -ENODEV_, -EPERM_ }, NULL },
    { "A21", "CLAIM edu from a fork child (not a driver)", C_CHILD, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_EXACT, EDU_BYID, C_UDRV,
      { -EPERM_, -EPERM_, -ENODEV_, -EPERM_ }, NULL },
    { "A22", "INFO handle 0 (stdin)", C_UDRV, UDRV_OP_INFO, H_STDIN, A_BUF, SZ_EXACT, 0, 0, 0, ALL(-EBADF_), NULL },
    { "A23", "INFO handle 31 (unused fd)", C_UDRV, UDRV_OP_INFO, H_FD31, A_BUF, SZ_EXACT, 0, 0, 0, ALL(-EBADF_), NULL },
    { "A24", "INFO handle 32 (past the fd table)", C_UDRV, UDRV_OP_INFO, H_FD32, A_BUF, SZ_EXACT, 0, 0, 0, ALL(-EBADF_), NULL },
    { "A25", "INFO handle -1", C_UDRV, UDRV_OP_INFO, H_NEG, A_BUF, SZ_EXACT, 0, 0, 0, ALL(-EBADF_), NULL },
    { "A26", "INFO handle 2^40", C_UDRV, UDRV_OP_INFO, H_HUGE, A_BUF, SZ_EXACT, 0, 0, 0, ALL(-EBADF_), NULL },
    { "A27", "MAP_BAR handle 0 (stdin)", C_UDRV, UDRV_OP_MAP_BAR, H_STDIN, A_BUF, SZ_EXACT, 0, 0, 0, ALL(-EBADF_), NULL },
    { "A28", "DMA_ALLOC handle 31", C_UDRV, UDRV_OP_DMA_ALLOC, H_FD31, A_BUF, SZ_EXACT, 4096, UDRV_DMA_BIDIR, 0, ALL(-EBADF_), NULL },
    { "A29", "RELEASE handle 31", C_UDRV, UDRV_OP_RELEASE, H_FD31, A_NULL, 0, 0, 0, 0, ALL(-EBADF_), NULL },
    { "A30", "RESET size 8", C_UDRV, UDRV_OP_RESET, H_STDIN, A_BUF, 8, 0, 0, 0, ALL(-EINVAL_), NULL },
};

/* Table B: on a claim of edu. Columns: DMA rights, MMIO only. */
static const row_t g_b[] = {
    { "B01", "CLAIM id edu again (all matches claimed)", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_EXACT, EDU_BYID, 0, DM(-EBUSY_, -EBUSY_), NULL },
    { "B02", "CLAIM bdf 00:04.0 again", C_UDRV, UDRV_OP_CLAIM, H_NONE, A_BUF, SZ_EXACT, BDF(0, 4, 0), 0, DM(-EBUSY_, -EBUSY_), NULL },
    { "B03", "INFO size 0", C_UDRV, UDRV_OP_INFO, H_CLAIM, A_BUF, 0, 0, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B04", "INFO size sizeof-1", C_UDRV, UDRV_OP_INFO, H_CLAIM, A_BUF, SZ_MINUS1, 0, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B05", "INFO arg NULL", C_UDRV, UDRV_OP_INFO, H_CLAIM, A_NULL, SZ_EXACT, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B06", "INFO arg kernel half", C_UDRV, UDRV_OP_INFO, H_CLAIM, A_KERNEL, SZ_EXACT, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B07", "INFO arg read-only page", C_UDRV, UDRV_OP_INFO, H_CLAIM, A_RO, SZ_EXACT, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B08", "MAP_BAR bar 6", C_UDRV, UDRV_OP_MAP_BAR, H_CLAIM, A_BUF, SZ_EXACT, 6, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B09", "MAP_BAR bar 0xffffffff", C_UDRV, UDRV_OP_MAP_BAR, H_CLAIM, A_BUF, SZ_EXACT, 0xFFFFFFFFull, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B10", "MAP_BAR bar 1 (absent on edu)", C_UDRV, UDRV_OP_MAP_BAR, H_CLAIM, A_BUF, SZ_EXACT, 1, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B11", "MAP_BAR bar 5 (absent on edu)", C_UDRV, UDRV_OP_MAP_BAR, H_CLAIM, A_BUF, SZ_EXACT, 5, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B12", "MAP_BAR flags 1", C_UDRV, UDRV_OP_MAP_BAR, H_CLAIM, A_BUF, SZ_EXACT, 0, 1, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B13", "MAP_BAR bar 0, result struct read-only", C_UDRV, UDRV_OP_MAP_BAR, H_CLAIM, A_RO, SZ_EXACT, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B14", "MAP_BAR bar 0 (nothing left mapped by B13)", C_UDRV, UDRV_OP_MAP_BAR, H_CLAIM, A_BUF, SZ_EXACT, 0, 0, 0, DM(0, 0), NULL },
    { "B15", "MAP_BAR bar 0 again", C_UDRV, UDRV_OP_MAP_BAR, H_CLAIM, A_BUF, SZ_EXACT, 0, 0, 0, DM(-EBUSY_, -EBUSY_), NULL },
    { "B16", "CFG_READ width 0", C_UDRV, UDRV_OP_CFG_READ, H_CLAIM, A_BUF, SZ_EXACT, 0, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B17", "CFG_READ width 3", C_UDRV, UDRV_OP_CFG_READ, H_CLAIM, A_BUF, SZ_EXACT, 0, 3, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B18", "CFG_READ width 8", C_UDRV, UDRV_OP_CFG_READ, H_CLAIM, A_BUF, SZ_EXACT, 0, 8, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B19", "CFG_READ off 0x1000", C_UDRV, UDRV_OP_CFG_READ, H_CLAIM, A_BUF, SZ_EXACT, 0x1000, 4, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B20", "CFG_READ off 0x2 width 4 (unaligned)", C_UDRV, UDRV_OP_CFG_READ, H_CLAIM, A_BUF, SZ_EXACT, 0x2, 4, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B21", "CFG_READ off 0x1 width 2 (unaligned)", C_UDRV, UDRV_OP_CFG_READ, H_CLAIM, A_BUF, SZ_EXACT, 0x1, 2, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B22", "CFG_READ result struct read-only", C_UDRV, UDRV_OP_CFG_READ, H_CLAIM, A_RO, SZ_EXACT, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B23", "CFG_WRITE 0x00 (vendor/device id)", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, 0x00, 4, 0, DM(-EPERM_, -EPERM_), NULL },
    { "B24", "CFG_WRITE 0x10 (BAR 0)", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, 0x10, 4, 0xFFFFFFFFull, DM(-EPERM_, -EPERM_), NULL },
    { "B25", "CFG_WRITE 0x24 (BAR 5)", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, 0x24, 4, 0, DM(-EPERM_, -EPERM_), NULL },
    { "B26", "CFG_WRITE 0x30 (expansion ROM)", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, 0x30, 4, 1, DM(-EPERM_, -EPERM_), NULL },
    { "B27", "CFG_WRITE 0x3c width 1 (interrupt line)", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, 0x3C, 1, 5, DM(-EPERM_, -EPERM_), NULL },
    { "B28", "CFG_WRITE MSI capability (message control)", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, V_MSI_CAP, 2, 0x0081, DM(-EPERM_, -EPERM_), NULL },
    { "B29", "CFG_WRITE 0x100 (extended space)", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, 0x100, 4, 0, DM(-EPERM_, -EPERM_), NULL },
    { "B30", "CFG_WRITE 0xffc", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, 0xFFC, 4, 0, DM(-EPERM_, -EPERM_), NULL },
    { "B31", "CFG_WRITE off 0x1000", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, 0x1000, 4, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B32", "CFG_WRITE width 3", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, 0x04, 3, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B33", "CFG_WRITE command I/O space bit", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, 0x04, 2, 0x0007, DM(-EPERM_, -EPERM_), NULL },
    { "B34", "CFG_WRITE command MEM|BUSMASTER", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, 0x04, 2, 0x0006, DM(0, -EPERM_), NULL },
    { "B35", "CFG_WRITE status 0x06 (ignored)", C_UDRV, UDRV_OP_CFG_WRITE, H_CLAIM, A_BUF, SZ_EXACT, 0x06, 2, 0xFFFF, DM(0, 0), NULL },
    { "B36", "DMA_ALLOC size 0", C_UDRV, UDRV_OP_DMA_ALLOC, H_CLAIM, A_BUF, SZ_EXACT, 0, UDRV_DMA_BIDIR, 0, DM(-EINVAL_, -ENODEV_), NULL },
    { "B37", "DMA_ALLOC size 4 MiB + 4 KiB", C_UDRV, UDRV_OP_DMA_ALLOC, H_CLAIM, A_BUF, SZ_EXACT, (4ull << 20) + 4096, UDRV_DMA_BIDIR, 0, DM(-EINVAL_, -ENODEV_), NULL },
    { "B38", "DMA_ALLOC size 2^64-4096 (round-up overflow)", C_UDRV, UDRV_OP_DMA_ALLOC, H_CLAIM, A_BUF, SZ_EXACT, ~0xFFFull, UDRV_DMA_BIDIR, 0, DM(-EINVAL_, -ENODEV_), NULL },
    { "B39", "DMA_ALLOC size 2^64-1", C_UDRV, UDRV_OP_DMA_ALLOC, H_CLAIM, A_BUF, SZ_EXACT, ~0ull, UDRV_DMA_BIDIR, 0, DM(-EINVAL_, -ENODEV_), NULL },
    { "B40", "DMA_ALLOC dir 0", C_UDRV, UDRV_OP_DMA_ALLOC, H_CLAIM, A_BUF, SZ_EXACT, 4096, 0, 0, DM(-EINVAL_, -ENODEV_), NULL },
    { "B41", "DMA_ALLOC dir 4", C_UDRV, UDRV_OP_DMA_ALLOC, H_CLAIM, A_BUF, SZ_EXACT, 4096, 4, 0, DM(-EINVAL_, -ENODEV_), NULL },
    { "B42", "DMA_ALLOC flags 1", C_UDRV, UDRV_OP_DMA_ALLOC, H_CLAIM, A_BUF, SZ_EXACT, 4096, UDRV_DMA_BIDIR, 1, DM(-EINVAL_, -ENODEV_), NULL },
    { "B43", "DMA_ALLOC 128 KiB (manifest dma_max 64 KiB)", C_UDRV, UDRV_OP_DMA_ALLOC, H_CLAIM, A_BUF, SZ_EXACT, 128ull << 10, UDRV_DMA_BIDIR, 0, DM(-ENOMEM_, -ENODEV_), NULL },
    { "B44", "DMA_ALLOC result struct read-only", C_UDRV, UDRV_OP_DMA_ALLOC, H_CLAIM, A_RO, SZ_EXACT, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B45", "DMA_ALLOC 64 KiB (all of dma_max)", C_UDRV, UDRV_OP_DMA_ALLOC, H_CLAIM, A_BUF, SZ_EXACT, 64ull << 10, UDRV_DMA_BIDIR, 0, DM(0, -ENODEV_), NULL },
    { "B46", "DMA_ALLOC 4 KiB more (dma_max reached)", C_UDRV, UDRV_OP_DMA_ALLOC, H_CLAIM, A_BUF, SZ_EXACT, 4096, UDRV_DMA_BIDIR, 0, DM(-ENOMEM_, -ENODEV_), NULL },
    { "B47", "DMA_FREE uva 0", C_UDRV, UDRV_OP_DMA_FREE, H_CLAIM, A_BUF, SZ_EXACT, 0, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B48", "DMA_FREE uva inside a buffer (not its start)", C_UDRV, UDRV_OP_DMA_FREE, H_CLAIM, A_DMA_PLUS, SZ_EXACT, 0, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B49", "DMA_FREE size sizeof+1", C_UDRV, UDRV_OP_DMA_FREE, H_CLAIM, A_DMA, SZ_PLUS1, 0, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B50", "munmap of a DMA buffer page", C_MUNMAP, 0, H_NONE, A_DMA, 4096, 0, 0, 0, DM(-EINVAL_, SKIP), NULL },
    { "B51", "mprotect of a DMA buffer page", C_MPROTECT, 0, H_NONE, A_DMA, 4096, 0, 0, 0, DM(-EINVAL_, SKIP), NULL },
    { "B52", "munmap of a BAR page", C_MUNMAP, 0, H_NONE, A_BAR, 4096, 0, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B53", "mprotect of a BAR page", C_MPROTECT, 0, H_NONE, A_BAR, 4096, 0, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B54", "mmap of the claim fd", C_MMAP_FD, 0, H_CLAIM, A_NULL, 4096, 0, 0, 0, DM(-ENODEV_, -ENODEV_), NULL },
    { "B55", "DMA_FREE the 64 KiB buffer", C_UDRV, UDRV_OP_DMA_FREE, H_CLAIM, A_DMA, SZ_EXACT, 0, 0, 0, DM(0, SKIP), NULL },
    { "B56", "DMA_FREE the same buffer again", C_UDRV, UDRV_OP_DMA_FREE, H_CLAIM, A_DMA, SZ_EXACT, 0, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B57", "IRQ_SETUP nvec 0", C_UDRV, UDRV_OP_IRQ_SETUP, H_CLAIM, A_BUF, SZ_EXACT, 0, 0, 0, DM(-EINVAL_, -EPERM_), NULL },
    { "B58", "IRQ_SETUP nvec 5 (manifest irq_max 4)", C_UDRV, UDRV_OP_IRQ_SETUP, H_CLAIM, A_BUF, SZ_EXACT, 5, 0, 0, DM(-EINVAL_, -EPERM_), NULL },
    { "B59", "IRQ_SETUP nvec 33", C_UDRV, UDRV_OP_IRQ_SETUP, H_CLAIM, A_BUF, SZ_EXACT, 33, 0, 0, DM(-EINVAL_, -EPERM_), NULL },
    { "B60", "IRQ_SETUP nvec 0xffffffff", C_UDRV, UDRV_OP_IRQ_SETUP, H_CLAIM, A_BUF, SZ_EXACT, 0xFFFFFFFFull, 0, 0, DM(-EINVAL_, -EPERM_), NULL },
    { "B61", "IRQ_SETUP nvec 2 (edu has one MSI vector)", C_UDRV, UDRV_OP_IRQ_SETUP, H_CLAIM, A_BUF, SZ_EXACT, 2, 0, 0, DM(-EINVAL_, -EPERM_), NULL },
    { "B62", "IRQ_SETUP result struct read-only", C_UDRV, UDRV_OP_IRQ_SETUP, H_CLAIM, A_RO, SZ_EXACT, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B63", "IRQ_SETUP nvec 1 (nothing left set up by B62)", C_UDRV, UDRV_OP_IRQ_SETUP, H_CLAIM, A_BUF, SZ_EXACT, 1, 0, 0, DM(0, -EPERM_), NULL },
    { "B64", "IRQ_SETUP nvec 1 again", C_UDRV, UDRV_OP_IRQ_SETUP, H_CLAIM, A_BUF, SZ_EXACT, 1, 0, 0, DM(-EBUSY_, -EPERM_), NULL },
    { "B65", "read len 15", C_READ, 0, H_CLAIM, A_BUF, 15, 0, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B66", "read len 0 (generic SYS_READ check)", C_READ, 0, H_CLAIM, A_BUF, 0, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B67", "write len 3", C_WRITE, 0, H_CLAIM, A_BUF, 3, 0, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B68", "write len 0 (generic SYS_WRITE check)", C_WRITE, 0, H_CLAIM, A_BUF, 0, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B69", "write buffer NULL", C_WRITE, 0, H_CLAIM, A_NULL, 8, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B70", "write buffer 0x1000 (unmapped user page)", C_WRITE, 0, H_CLAIM, A_LOW, 8, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B71", "write buffer kernel half", C_WRITE, 0, H_CLAIM, A_KERNEL, 8, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B72", "read O_NONBLOCK, nothing pending", C_READ, 0, H_CLAIM, A_BUF, 16, 0, 0, 0, DM(-EAGAIN_, -EAGAIN_), pre_nonblock },
    { "B73", "read buffer kernel half", C_READ, 0, H_CLAIM, A_KERNEL, 16, 0, 0, 0, DM(-EFAULT_, -EFAULT_), NULL },
    { "B74", "read buffer read-only page, interrupt pending", C_READ, 0, H_CLAIM, A_RO, 16, 0, 0, 0, DM(-EFAULT_, -EAGAIN_), pre_irq_pending },
    { "B75", "write ack (after B74)", C_WRITE, 0, H_CLAIM, A_BUF, 8, 1, 0, 0, DM(8, 8), pre_irq_ack },
    { "B76", "RESET (edu has no reset)", C_UDRV, UDRV_OP_RESET, H_CLAIM, A_NULL, 0, 0, 0, 0, DM(-EOPNOTSUPP_, -EOPNOTSUPP_), NULL },
    { "B77", "INFO from a fork child (not the owner)", C_CHILD, UDRV_OP_INFO, H_CLAIM, A_BUF, SZ_EXACT, 0, 0, C_UDRV, DM(-EPERM_, -EPERM_), NULL },
    { "B78", "read from a fork child", C_CHILD, 0, H_CLAIM, A_BUF, 16, 0, 0, C_READ, DM(-EPERM_, -EPERM_), NULL },
    { "B79", "write from a fork child", C_CHILD, 0, H_CLAIM, A_BUF, 8, 1, 0, C_WRITE, DM(-EPERM_, -EPERM_), NULL },
    { "B80", "RELEASE from a fork child", C_CHILD, UDRV_OP_RELEASE, H_CLAIM, A_NULL, 0, 0, 0, C_UDRV, DM(-EPERM_, -EPERM_), NULL },
    { "B81", "RELEASE size 4", C_UDRV, UDRV_OP_RELEASE, H_CLAIM, A_BUF, 4, 0, 0, 0, DM(-EINVAL_, -EINVAL_), NULL },
    { "B82", "RELEASE", C_UDRV, UDRV_OP_RELEASE, H_CLAIM, A_NULL, 0, 0, 0, 0, DM(0, 0), NULL },
    { "B83", "INFO after RELEASE", C_UDRV, UDRV_OP_INFO, H_CLAIM, A_BUF, SZ_EXACT, 0, 0, 0, DM(-ENODEV_, -ENODEV_), NULL },
    { "B84", "MAP_BAR after RELEASE", C_UDRV, UDRV_OP_MAP_BAR, H_CLAIM, A_BUF, SZ_EXACT, 0, 0, 0, DM(-ENODEV_, -ENODEV_), NULL },
    { "B85", "read after RELEASE", C_READ, 0, H_CLAIM, A_BUF, 16, 0, 0, 0, DM(-ENODEV_, -ENODEV_), NULL },
    { "B86", "write after RELEASE", C_WRITE, 0, H_CLAIM, A_BUF, 8, 1, 0, 0, DM(-ENODEV_, -ENODEV_), NULL },
    { "B87", "RELEASE again", C_UDRV, UDRV_OP_RELEASE, H_CLAIM, A_NULL, 0, 0, 0, 0, DM(-ENODEV_, -ENODEV_), NULL },
    { "B88", "INFO on the closed claim fd", C_UDRV, UDRV_OP_INFO, H_CLAIM, A_BUF, SZ_EXACT, 0, 0, 0, DM(-EBADF_, -EBADF_), pre_close },
};

/* ------------------------------------------------------------ running */
static union {
    struct udrv_claim cl; struct udrv_info in; struct udrv_map_bar mb;
    struct udrv_cfg cf; struct udrv_dma dm; struct udrv_irq iq; struct udrv_ack ak;
    struct udrv_events ev; uint8_t raw[256];
} g_arg;

static unsigned long op_size(long op) {
    switch (op) {
    case UDRV_OP_CLAIM: return sizeof(struct udrv_claim);
    case UDRV_OP_INFO: return sizeof(struct udrv_info);
    case UDRV_OP_MAP_BAR: return sizeof(struct udrv_map_bar);
    case UDRV_OP_CFG_READ: case UDRV_OP_CFG_WRITE: return sizeof(struct udrv_cfg);
    case UDRV_OP_DMA_ALLOC: case UDRV_OP_DMA_FREE: return sizeof(struct udrv_dma);
    case UDRV_OP_IRQ_SETUP: return sizeof(struct udrv_irq);
    default: return 0;
    }
}

/* The struct of A_BUF from the row's v0..v2. */
static void fill(const row_t* r) {
    memset(&g_arg, 0, sizeof(g_arg));
    long op = r->call == C_UDRV || r->call == C_EUID || (r->call == C_CHILD && r->v2 == C_UDRV)
                  ? (r->op & 0xFF) : -1;
    if (r->call == C_WRITE || (r->call == C_CHILD && r->v2 == C_WRITE)) { g_arg.ak.vec_mask = (uint32_t)r->v0; return; }
    switch (op) {
    case UDRV_OP_CLAIM:
        g_arg.cl.flags = (uint32_t)r->v0;
        if (r->v0 & UDRV_CLAIM_BY_ID) {
            g_arg.cl.vendor = (uint16_t)(r->v1 >> 16); g_arg.cl.device = (uint16_t)r->v1;
        } else {
            g_arg.cl.bus = (uint8_t)(r->v1 >> 16); g_arg.cl.dev = (uint8_t)(r->v1 >> 8); g_arg.cl.fn = (uint8_t)r->v1;
        }
        g_arg.cl.dma_mask = 0x0FFFFFFFull;
        break;
    case UDRV_OP_MAP_BAR: g_arg.mb.bar = (uint32_t)r->v0; g_arg.mb.flags = (uint32_t)r->v1; break;
    case UDRV_OP_CFG_READ: case UDRV_OP_CFG_WRITE:
        g_arg.cf.off = r->v0 == V_MSI_CAP ? (uint32_t)(g_msi_cap + 2) : (uint32_t)r->v0;
        g_arg.cf.width = (uint32_t)r->v1; g_arg.cf.val = (uint32_t)r->v2;
        break;
    case UDRV_OP_DMA_ALLOC:
        g_arg.dm.size = r->v0; g_arg.dm.dir = (uint32_t)r->v1; g_arg.dm.flags = (uint32_t)r->v2;
        break;
    case UDRV_OP_DMA_FREE:
        g_arg.dm.uva = r->a == A_DMA ? g_dma : r->a == A_DMA_PLUS ? g_dma + 4096 : r->v0;
        break;
    case UDRV_OP_IRQ_SETUP: g_arg.iq.nvec = (uint32_t)r->v0; break;
    default: break;
    }
}

static long handle_of(int h) {
    switch (h) {
    case H_CLAIM: return g_fd;
    case H_STDIN: return 0;
    case H_FD31: return 31;
    case H_FD32: return 32;
    case H_NEG: return -1;
    case H_HUGE: return 1L << 40;
    default: return 0;
    }
}

static void* arg_of(int a) {
    switch (a) {
    case A_BUF: case A_DMA_PLUS: return &g_arg;
    case A_NULL: return NULL;
    case A_LOW: return (void*)0x1000;
    case A_KERNEL: return (void*)0xFFFFFFFF80000000ull;
    case A_NONCANON: return (void*)0x0000800000000000ull;
    case A_TOP: return (void*)0x00007FFFFFFFFFF8ull;
    case A_RO: return (void*)k_ro;
    case A_BAR: return (void*)(uintptr_t)g_bar;
    case A_DMA: return (void*)(uintptr_t)g_dma;
    default: return NULL;
    }
}

/* DMA_FREE takes its uva inside the struct: A_DMA/A_DMA_PLUS pass &g_arg. */
static void* udrv_arg_of(const row_t* r) {
    if (r->op == UDRV_OP_DMA_FREE && (r->a == A_DMA || r->a == A_DMA_PLUS)) return &g_arg;
    return arg_of(r->a);
}

static unsigned long size_of(const row_t* r) {
    unsigned long s = op_size(r->op & 0xFF);
    switch (r->size) {
    case SZ_EXACT: return s;
    case SZ_MINUS1: return s - 1;
    case SZ_PLUS1: return s + 1;
    case SZ_HUGE: return ~0ul;
    default: return (unsigned long)r->size;
    }
}

static long do_call(const row_t* r, int how) {
    long h = handle_of(r->h);
    switch (how) {
    case C_UDRV: return secos_udrv(r->op, h, udrv_arg_of(r), size_of(r));
    case C_READ: return secos_syscall(SYS_READ_, h, (long)arg_of(r->a), r->size, 0, 0);
    case C_WRITE: return secos_syscall(SYS_WRITE_, h, (long)arg_of(r->a), r->size, 0, 0);
    case C_MUNMAP: return munmap(arg_of(r->a), (size_t)r->size);
    case C_MPROTECT: return mprotect(arg_of(r->a), (size_t)r->size, PROT_READ);
    case C_MMAP_FD: {
        void* p = secos_mmap(NULL, (size_t)r->size, PROT_READ, MAP_SHARED, (int)h, 0);
        if (p != MAP_FAILED) { munmap(p, (size_t)r->size); return 0; }
        return -errno;
    }
    default: return -9999;
    }
}

static long run_row(const row_t* r) {
    if (r->pre) r->pre();
    fill(r);
    if (r->call == C_EUID) {
        long s = secos_syscall(SYS_SETRESUID_, -1, 65534, -1, 0, 0);
        if (s != 0) return -9000 + s;
        long rc = do_call(r, C_UDRV);
        long b = secos_syscall(SYS_SETRESUID_, -1, 0, -1, 0, 0);
        if (b != 0) { say("[udrv_negtest] could not restore euid 0 (rc=%ld)\n", b); _exit(3); }
        return rc;
    }
    if (r->call == C_CHILD) {
        int pid = fork();
        if (pid < 0) return -9100;
        if (pid == 0) {
            long rc = do_call(r, (int)r->v2);
            _exit(rc < 0 && rc > -256 ? (int)-rc : rc == 0 ? 0 : 255);
        }
        int st = waitpid(pid);
        if (!WIFEXITED(st)) return -9200;
        int code = WEXITSTATUS(st);
        return code == 255 ? -9300 : -(long)code;
    }
    return do_call(r, r->call);
}

static void report(const row_t* r, long rc, int want) {
    char rb[24], wb[24];
    const char* rn = ename(rc);
    const char* wn = ename(want);
    if (!rn) { snprintf(rb, sizeof(rb), "%ld", rc); rn = rb; }
    if (!wn) { snprintf(wb, sizeof(wb), "%d", want); wn = wb; }
    if (want == SKIP) {
        g_skip++;
        say("[udrv_negtest] %s %s: SKIP (not applicable here)\n", r->id, r->name);
        return;
    }
    int ok = rc == want;
    if (ok) g_pass++; else g_fail++;
    say("[udrv_negtest] %s %s: %s rc=%s want=%s\n", r->id, r->name, ok ? "PASS" : "FAIL", rn, wn);
}

static void table(const row_t* t, int n) {
    for (int i = 0; i < n; i++) {
        const row_t* r = &t[i];
        int want = r->exp[g_cls];
        if (want == SKIP) { report(r, 0, SKIP); continue; }
        long rc = run_row(r);
        /* Remember what the stateful rows created. */
        if (rc == 0 && r->call == C_UDRV && r->a == A_BUF) {
            if (r->op == UDRV_OP_MAP_BAR) g_bar = g_arg.mb.uva;
            if (r->op == UDRV_OP_DMA_ALLOC) g_dma = g_arg.dm.uva;
        }
        /* A successful claim in table A would be a bug: give it back. */
        if (rc >= 0 && r->op == UDRV_OP_CLAIM && r->call == C_UDRV) {
            secos_udrv(UDRV_OP_RELEASE, rc, NULL, 0);
            close((int)rc);
        }
        report(r, rc, want);
    }
}

/* Offset of edu's MSI capability (id 0x05), or 0. */
static uint64_t find_msi_cap(void) {
    struct udrv_cfg c = { 0x34, 1, 0, 0 };
    if (secos_udrv(UDRV_OP_CFG_READ, g_fd, &c, sizeof(c)) != 0) return 0;
    uint32_t p = c.val & 0xFC;
    for (int hops = 0; p >= 0x40 && hops < 48; hops++) {
        struct udrv_cfg id = { p, 2, 0, 0 };
        if (secos_udrv(UDRV_OP_CFG_READ, g_fd, &id, sizeof(id)) != 0) return 0;
        if ((id.val & 0xFF) == 0x05) return p;
        p = (id.val >> 8) & 0xFC;
    }
    return 0;
}

static int g_env_ok = 1;
static void env_check(const char* what, int ok, long got, long want) {
    if (ok) g_pass++; else { g_fail++; g_env_ok = 0; }
    say("[udrv_negtest] env %s: %s got=%ld want=%ld\n", what, ok ? "PASS" : "FAIL", got, want);
}

static int claim_edu(void) {
    struct udrv_claim c;
    memset(&c, 0, sizeof(c));
    c.vendor = 0x1234; c.device = 0x11e8; c.flags = UDRV_CLAIM_BY_ID; c.dma_mask = 0x0FFFFFFFull;
    return (int)secos_udrv(UDRV_OP_CLAIM, 0, &c, sizeof(c));
}

int main(int argc, char** argv) {
    const char* env = NULL;
    if (argc == 3 && !strcmp(argv[1], "--env")) env = argv[2];
    int want_iommu = 0, want_unsafe = 0;
    if (!env) { say("usage: udrv_negtest --env translated|unsafe|noir|nommu|nodev|user\n"); return 2; }
    if (!strcmp(env, "translated")) { g_cls = CL_DMA; want_iommu = 1; }
    else if (!strcmp(env, "unsafe")) { g_cls = CL_DMA; want_unsafe = 1; }
    else if (!strcmp(env, "noir")) { g_cls = CL_MMIO; want_iommu = 1; }
    else if (!strcmp(env, "nommu")) g_cls = CL_MMIO;
    else if (!strcmp(env, "nodev")) g_cls = CL_NODEV;
    else if (!strcmp(env, "user")) g_cls = CL_USER;
    else { say("udrv_negtest: unknown environment %s\n", env); return 2; }
    say("[udrv_negtest] start pid=%d env=%s\n", getpid(), env);

    table(g_a, (int)(sizeof(g_a) / sizeof(g_a[0])));
    int rows = (int)(sizeof(g_a) / sizeof(g_a[0]));

    if (g_cls == CL_DMA || g_cls == CL_MMIO) {
        g_fd = claim_edu();
        env_check("claim", g_fd >= 0, g_fd, 3);
        if (g_fd >= 0) {
            struct udrv_info in;
            memset(&in, 0, sizeof(in));
            long r = secos_udrv(UDRV_OP_INFO, g_fd, &in, sizeof(in));
            env_check("info", r == 0, r, 0);
            env_check("info.dma_ok", in.dma_ok == (g_cls == CL_DMA), in.dma_ok, g_cls == CL_DMA);
            env_check("info.iommu", in.iommu == want_iommu, in.iommu, want_iommu);
            env_check("info.unsafe", in.unsafe == want_unsafe, in.unsafe, want_unsafe);
            g_msi_cap = find_msi_cap();
            env_check("msi-capability", g_msi_cap >= 0x40, (long)g_msi_cap, 0x40);
            if (g_env_ok) {
                table(g_b, (int)(sizeof(g_b) / sizeof(g_b[0])));
                rows += (int)(sizeof(g_b) / sizeof(g_b[0]));
                /* The device is usable again after every refusal above. */
                int fd = claim_edu();
                env_check("reclaim", fd >= 0, fd, 3);
                if (fd >= 0) {
                    long rr = secos_udrv(UDRV_OP_RELEASE, fd, NULL, 0);
                    env_check("reclaim-release", rr == 0, rr, 0);
                    close(fd);
                }
            }
        }
    }
    say("[udrv_negtest] RESULT rows=%d pass=%d fail=%d skip=%d\n", rows, g_pass, g_fail, g_skip);
    say("[udrv_negtest] %s pid=%d\n", g_fail ? "FAIL" : "PASS", getpid());
    return g_fail ? 1 : 0;
}
