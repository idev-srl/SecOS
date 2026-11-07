/*
 * csprng.c — kernel cryptographic random number generator (/dev/urandom).
 * SPDX-License-Identifier: MIT
 *
 * Replaces the tick-seeded xorshift that used to back /dev/random — fatal for
 * an SSH server (host keys and every ephemeral key exchange secret would have
 * come from ~a few thousand possible seeds).
 *
 * Design (hash-DRBG style, SHA-256 only, so no new primitive):
 *  - Entropy: RDSEED and RDRAND when CPUID reports them, plus TSC jitter (a few
 *    hundred rdtsc deltas across short busy loops) and the tick counter. The
 *    timer IRQ also stirs one TSC sample per tick into a small accumulator
 *    (csprng_stir_tsc), folded in at every reseed.
 *  - State: a 32-byte key K = SHA-256(previous K || fresh entropy).
 *  - Output: block_i = SHA-256(K || counter || i); after every request K is
 *    replaced by SHA-256(K || "rekey" || counter) (forward secrecy: a later
 *    state compromise does not reveal earlier output).
 *  - Reseed: at first use, then every CSPRNG_RESEED_BYTES of output, each time
 *    mixing new RDRAND/RDSEED/TSC input.
 * Without RDRAND/RDSEED (old CPUs, some VMs) only TSC jitter + IRQ timing feed
 * the pool; the boot log says so ("[RNG] ... TSC jitter only").
 */
#include "csprng.h"
#include "sha256.h"
#include "debugcon.h"
#include "../arch/x86/spinlock.h"

#define CSPRNG_RESEED_BYTES (1u << 20)

static uint8_t  g_key[32];
static uint64_t g_counter;
static uint64_t g_since_reseed;
static int      g_ready;
static int      g_has_rdrand, g_has_rdseed;
static volatile uint64_t g_irq_acc[8];
static volatile uint32_t g_irq_n;
static spinlock_t g_lock = SPINLOCK_INIT_CLASS(LK_S64_LEAF, "csprng");   /* SMP: one generator state */

static inline uint64_t rdtsc64(void){
    uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo;
}
static void cpuid(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d){
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static int rdrand64(uint64_t* v){
    for (int i = 0; i < 10; i++){ uint8_t ok; __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok)); if (ok) return 1; }
    return 0;
}
static int rdseed64(uint64_t* v){
    for (int i = 0; i < 100; i++){ uint8_t ok; __asm__ volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok)); if (ok) return 1; __asm__ volatile("pause"); }
    return 0;
}

/* Called from the timer IRQ: cheap, lock-free, never blocks. */
void csprng_stir_tsc(void){
    uint32_t n = g_irq_n++;
    g_irq_acc[n & 7] = (g_irq_acc[n & 7] << 7 | g_irq_acc[n & 7] >> 57) ^ rdtsc64();
}

static void reseed(void){
    extern uint64_t timer_get_ticks(void);
    sha256_ctx h; sha256_init(&h);
    sha256_update(&h, g_key, sizeof(g_key));
    uint64_t v;
    int hw = 0;
    for (int i = 0; i < 8; i++){
        if (g_has_rdseed && rdseed64(&v)){ sha256_update(&h, &v, sizeof(v)); hw++; }
        if (g_has_rdrand && rdrand64(&v)){ sha256_update(&h, &v, sizeof(v)); hw++; }
    }
    /* TSC jitter: timing of short loops varies with caches, pipelines, SMIs and
     * the hypervisor; hash every delta (no entropy estimate is claimed). */
    uint64_t prev = rdtsc64();
    for (int i = 0; i < 256; i++){
        volatile uint32_t spin = 0;
        for (uint32_t k = 0; k < (uint32_t)(prev & 0x3f) + 16; k++) spin += k;
        uint64_t t = rdtsc64();
        uint64_t d = t - prev; prev = t;
        sha256_update(&h, &d, sizeof(d));
    }
    uint64_t misc[3] = { timer_get_ticks(), rdtsc64(), g_counter };
    sha256_update(&h, misc, sizeof(misc));
    uint64_t acc[8]; for (int i = 0; i < 8; i++) acc[i] = g_irq_acc[i];
    sha256_update(&h, acc, sizeof(acc));
    sha256_final(&h, g_key);
    g_since_reseed = 0;
    (void)hw;
}

static void init_locked(void){
    if (g_ready) return;
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t maxleaf = a;
    cpuid(1, 0, &a, &b, &c, &d);
    g_has_rdrand = (c >> 30) & 1;
    if (maxleaf >= 7){ cpuid(7, 0, &a, &b, &c, &d); g_has_rdseed = (b >> 18) & 1; }
    reseed();
    g_ready = 1;
    debugcon_writestring("[RNG] kernel CSPRNG (SHA-256 DRBG) seeded from ");
    if (g_has_rdseed || g_has_rdrand){
        if (g_has_rdseed) debugcon_writestring("RDSEED+");
        if (g_has_rdrand) debugcon_writestring("RDRAND+");
        debugcon_writestring("TSC jitter\n");
    } else {
        debugcon_writestring("TSC jitter only (no RDRAND/RDSEED: weaker at early boot)\n");
    }
}
void csprng_init(void){
    uint64_t f = spin_lock_irqsave(&g_lock);
    init_locked();
    spin_unlock_irqrestore(&g_lock, f);
}

/* [M52] One request: at most CSPRNG_CHUNK bytes per lock hold. The lock is
 * irqsave (the timer IRQ stirs the pool), so a large /dev/random read used
 * to keep interrupts off for its whole length (64 KiB = 2048 SHA-256 blocks,
 * milliseconds without a tick on that CPU) and every other caller waiting.
 * Each chunk is its own request, with its own rekey. */
#define CSPRNG_CHUNK 256
static void csprng_get_chunk(void* out, size_t len);
void csprng_get(void* out, size_t len){
    uint8_t* o = (uint8_t*)out;
    while (len){
        size_t n = len < CSPRNG_CHUNK ? len : CSPRNG_CHUNK;
        csprng_get_chunk(o, n);
        o += n; len -= n;
    }
}

static void csprng_get_chunk(void* out, size_t len){
    uint64_t f = spin_lock_irqsave(&g_lock);
    init_locked();
    if (g_since_reseed >= CSPRNG_RESEED_BYTES) reseed();
    uint8_t* o = (uint8_t*)out;
    uint64_t req = ++g_counter;
    uint64_t blk = 0;
    while (len){
        uint8_t dg[32];
        sha256_ctx h; sha256_init(&h);
        sha256_update(&h, g_key, sizeof(g_key));
        sha256_update(&h, &req, sizeof(req));
        sha256_update(&h, &blk, sizeof(blk));
        sha256_final(&h, dg);
        size_t n = len < sizeof(dg) ? len : sizeof(dg);
        for (size_t i = 0; i < n; i++) o[i] = dg[i];
        o += n; len -= n; blk++;
        g_since_reseed += n;
    }
    /* Rekey: forward secrecy for everything returned so far. */
    sha256_ctx h; sha256_init(&h);
    sha256_update(&h, g_key, sizeof(g_key));
    sha256_update(&h, "rekey", 5);
    sha256_update(&h, &req, sizeof(req));
    sha256_final(&h, g_key);
    spin_unlock_irqrestore(&g_lock, f);
}

uint32_t csprng_u32(void){ uint32_t v; csprng_get(&v, sizeof(v)); return v; }
