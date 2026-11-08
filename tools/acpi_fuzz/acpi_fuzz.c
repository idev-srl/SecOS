/*
 * tools/acpi_fuzz/acpi_fuzz.c - host harness for arch/x86/acpi_dmar.c
 *
 * Builds the kernel DMAR/IVRS parser unchanged against a stub ACPI table
 * lookup and a stub PCI config space.
 *
 *   make -C tools/acpi_fuzz
 *   tools/acpi_fuzz/acpi_fuzz dump TABLE.bin            parse + summary
 *   tools/acpi_fuzz/acpi_fuzz fuzz ITER SEED TABLE.bin... fuzz
 *
 * `dump` feeds one table (DMAR or IVRS, by signature) to acpi_dmar_init(),
 * prints the kernel's [IOMMU] log on stderr and one `S ...` summary line per
 * parsed object on stdout (tools/acpi_tables.py check compares them with the
 * values it encoded). Config reads answer from the fixed topology described
 * in tools/acpi_tables.py.
 *
 * `fuzz` mutates the seed tables (bit flips, random bytes, length and count
 * fields, truncation, extension, splicing, checksum fix-up) and randomizes
 * the PCI topology (bridge loops, bogus bus numbers, absent functions) for
 * every iteration, then runs the parsers, the boot entry and every lookup
 * helper. Each input lives in an exact-size heap buffer, so ASan catches any
 * read past the table. Iteration i uses seed SEED+i: rerun one with
 * `fuzz 1 SEED+i ...`. A crash, UB report or hang (SIGALRM, 2 s) fails.
 *
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "acpi.h"
#include "acpi_dmar.h"

int g_fuzz_verbose = 0;

/* ------------------------------------------------ stub ACPI table lookup */
static const uint8_t* g_tbl;
static uint32_t       g_tbl_len;

const uint8_t* acpi_table_find(const char* sig, uint32_t* len){
    if(len) *len = 0;
    if(!g_tbl || g_tbl_len < 4 || memcmp(g_tbl, sig, 4)) return NULL;
    if(len) *len = g_tbl_len;
    return g_tbl;
}

/* ------------------------------------------------- stub PCI config space */
typedef struct {
    uint16_t seg; uint8_t bus, dev, fn;
    uint16_t vendor;
    uint8_t  hdr, cls;          /* header type, class code               */
    uint8_t  sec, sub;          /* bridges                               */
} fake_fn_t;
#define MAXFN 64
static fake_fn_t g_fns[MAXFN];
static int       g_nfn;
static int       g_absent_pct;   /* fuzz: random reads fail              */
static uint64_t  g_reads;

static uint64_t g_rng;
static uint32_t rnd(void){
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}

uint32_t acpi_dmar_cfg_read32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off){
    g_reads++;
    if(dev > 31 || fn > 7 || (off & 3)){
        fprintf(stderr, "bad cfg read %u:%u.%u+%u\n", bus, dev, fn, off);
        abort();
    }
    if(g_absent_pct && (int)(rnd() % 100) < g_absent_pct) return 0xFFFFFFFFu;
    for(int i = 0; i < g_nfn; i++){
        const fake_fn_t* f = &g_fns[i];
        if(f->seg != seg || f->bus != bus || f->dev != dev || f->fn != fn) continue;
        switch(off){
            case 0x00: return 0x10000000u | f->vendor;
            case 0x08: return (uint32_t)f->cls << 24;
            case 0x0C: return (uint32_t)f->hdr << 16;
            case 0x18: return (uint32_t)f->bus | ((uint32_t)f->sec << 8) | ((uint32_t)f->sub << 16);
            default:   return 0;
        }
    }
    return 0xFFFFFFFFu;
}

static void add_fn(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t hdr, uint8_t cls,
                   uint8_t sec, uint8_t sub){
    if(g_nfn >= MAXFN) return;
    fake_fn_t* f = &g_fns[g_nfn++];
    f->seg = seg; f->bus = bus; f->dev = dev; f->fn = fn; f->vendor = 0x8086;
    f->hdr = hdr; f->cls = cls; f->sec = sec; f->sub = sub;
}

static void fixed_topology(void){
    g_nfn = 0; g_absent_pct = 0;
    add_fn(0, 0, 0x00, 0, 0, 0x06, 0, 0);   /* host bridge          */
    add_fn(0, 0, 0x02, 0, 0, 0x03, 0, 0);   /* display              */
    add_fn(0, 0, 0x14, 0, 0, 0x0C, 0, 0);   /* USB                  */
    add_fn(0, 0, 0x1C, 0, 1, 0x06, 1, 2);   /* root port, buses 1-2 */
    add_fn(0, 1, 0x00, 0, 1, 0x06, 2, 2);   /* bridge, bus 2        */
    add_fn(0, 2, 0x00, 0, 0, 0x02, 0, 0);   /* NIC                  */
    add_fn(0, 0, 0x1F, 0, 0x80, 0x06, 0, 0);/* ISA (multi-function) */
}

static void random_topology(void){
    g_nfn = 0;
    g_absent_pct = (rnd() % 4 == 0) ? (int)(rnd() % 30) : 0;
    int n = (int)(rnd() % MAXFN);
    for(int i = 0; i < n; i++){
        uint8_t bus = (uint8_t)((rnd() & 1) ? rnd() % 4 : rnd());
        uint8_t hdr = (uint8_t)((rnd() % 3 == 0) ? 1 : (rnd() % 8 == 0 ? rnd() : 0));
        add_fn((uint16_t)(rnd() % 8 == 0 ? rnd() % 3 : 0), bus, (uint8_t)(rnd() % 32), (uint8_t)(rnd() % 8),
               hdr, (uint8_t)(rnd() % 6 == 0 ? 3 : rnd()),
               (uint8_t)((rnd() & 1) ? bus + 1 + rnd() % 3 : rnd()), (uint8_t)rnd());
    }
}

/* ---------------------------------------------------------------- dump */
static void pbdf(char* b, uint16_t v){ sprintf(b, "%02x:%02x.%x", v >> 8, (v >> 3) & 0x1F, v & 7); }

static const char* sc_name(uint8_t t){
    static const char* n[] = { "?", "dev", "bridge", "ioapic", "hpet", "acpi" };
    return t <= 5 ? n[t] : "?";
}
static const char* dev_name(uint8_t k){
    static const char* n[] = { "?", "all", "select", "range", "alias", "ext", "special", "acpihid" };
    return k <= 7 ? n[k] : "?";
}

static void dump_scopes(const acpi_dmar_t* d, uint16_t first, uint16_t count){
    char b[16];
    for(uint32_t k = 0; k < count; k++){
        const dmar_scope_t* s = &d->scopes[first + k];
        pbdf(b, s->bdf);
        printf("S SC %s id=%u bdf=%s res=%u", sc_name(s->type), s->enum_id, b, s->resolved);
        if(s->type == DMAR_SCOPE_BRIDGE && s->resolved) printf(" bus=%02x-%02x", s->sec_bus, s->sub_bus);
        if(s->is_display) printf(" disp");
        printf("\n");
    }
}

static void dump_dmar(void){
    const acpi_dmar_t* d = acpi_dmar_get();
    char b[16];
    if(!d->present) return;
    printf("S DMAR haw=%u flags=0x%x drhd=%u rmrr=%u atsr=%u rhsa=%u satc=%u andd=%u inv=%u drop=%u trunc=%u csum=%u\n",
           d->haw, d->flags, d->drhd_count, d->rmrr_count, d->atsr_count, d->rhsa_count, d->satc_count,
           d->andd_count, d->invalid, d->dropped, d->truncated, d->checksum_bad);
    for(uint32_t i = 0; i < d->drhd_count; i++){
        const dmar_drhd_t* u = &d->drhd[i];
        printf("S DRHD %u base=0x%llx seg=%u all=%u gfx=%u scopes=%u inc=%u", i,
               (unsigned long long)u->reg_base, u->segment, u->include_all, u->gfx_only,
               u->scope_count, u->scopes_incomplete);
        if(u->has_rhsa) printf(" node=%u", u->proximity);
        printf("\n");
        dump_scopes(d, u->scope_first, u->scope_count);
    }
    for(uint32_t i = 0; i < d->rmrr_count; i++){
        const dmar_rmrr_t* r = &d->rmrr[i];
        printf("S RMRR %u 0x%llx-0x%llx seg=%u scopes=%u inc=%u\n", i, (unsigned long long)r->base,
               (unsigned long long)r->limit, r->segment, r->scope_count, r->scopes_incomplete);
        dump_scopes(d, r->scope_first, r->scope_count);
    }
    static const uint16_t probe[] = { DMAR_BDF(2,0,0), DMAR_BDF(0,3,0), DMAR_BDF(0,2,0), DMAR_BDF(0,0x14,0) };
    for(unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++){
        const dmar_drhd_t* u = acpi_dmar_drhd_for(0, probe[i]);
        pbdf(b, probe[i]);
        if(u) printf("S FOR %s -> %d\n", b, (int)(u - d->drhd));
        else  printf("S FOR %s -> none\n", b);
        uint32_t first = 0;
        if(acpi_dmar_rmrr_covers(0, probe[i], &first)) printf("S RMRRCOV %s -> %u\n", b, first);
    }
    for(int id = 0; id < 4; id++){
        uint16_t sid;
        if(acpi_dmar_ioapic_sid((uint8_t)id, &sid) == 0){ pbdf(b, sid); printf("S IOAPIC%d %s\n", id, b); }
        if(acpi_dmar_hpet_sid((uint8_t)id, &sid) == 0){ pbdf(b, sid); printf("S HPET%d %s\n", id, b); }
    }
}

static void dump_ivrs(void){
    const acpi_ivrs_t* v = acpi_ivrs_get();
    char b[16], c[16];
    if(!v->present) return;
    printf("S IVRS ivinfo=0x%x ivhd=%u ivmd=%u primary=0x%x inv=%u drop=%u trunc=%u csum=%u\n",
           v->ivinfo, v->ivhd_count, v->ivmd_count, v->primary_type, v->invalid, v->dropped,
           v->truncated, v->checksum_bad);
    for(uint32_t i = 0; i < v->ivhd_count; i++){
        const ivrs_ivhd_t* h = &v->ivhd[i];
        pbdf(b, h->devid);
        printf("S IVHD %u type=0x%x prim=%u dev=%s base=0x%llx seg=%u n=%u\n", i, h->type, h->primary, b,
               (unsigned long long)h->base, h->segment, h->dev_count);
        if(!h->primary) continue;
        for(uint32_t k = 0; k < h->dev_count; k++){
            const ivrs_dev_t* e = &v->devs[h->dev_first + k];
            pbdf(b, e->start); pbdf(c, e->end);
            printf("S DEV %s %s-%s", dev_name(e->kind), b, c);
            if(e->kind == IVRS_DEV_ALIAS){ pbdf(b, e->alias); printf(" ->%s", b); }
            if(e->kind == IVRS_DEV_SPECIAL) printf(" v=%u h=%u", e->variety, e->handle);
            printf("\n");
        }
    }
    for(uint32_t i = 0; i < v->ivmd_count; i++){
        const ivrs_ivmd_t* m = &v->ivmd[i];
        pbdf(b, m->start); pbdf(c, m->end);
        printf("S IVMD %u type=0x%x %s-%s 0x%llx+0x%llx\n", i, m->type, b, c,
               (unsigned long long)m->base, (unsigned long long)m->length);
    }
    static const uint16_t probe[] = { DMAR_BDF(1,5,0), DMAR_BDF(3,2,0), DMAR_BDF(5,0,0) };
    for(unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++){
        uint16_t al = 0;
        const ivrs_ivhd_t* h = acpi_ivrs_ivhd_for(0, probe[i], &al);
        pbdf(b, probe[i]);
        if(h){ pbdf(c, al); printf("S LOOKUP %s -> %d alias=%s\n", b, (int)(h - v->ivhd), c); }
        else printf("S LOOKUP %s -> none\n", b);
    }
    for(int id = 0; id < 4; id++){
        uint16_t dv;
        if(acpi_ivrs_special_devid(IVRS_SPECIAL_IOAPIC, (uint8_t)id, &dv) == 0){ pbdf(b, dv); printf("S SPECIAL ioapic%d %s\n", id, b); }
        if(acpi_ivrs_special_devid(IVRS_SPECIAL_HPET, (uint8_t)id, &dv) == 0){ pbdf(b, dv); printf("S SPECIAL hpet%d %s\n", id, b); }
    }
}

static uint8_t* load(const char* path, uint32_t* len){
    FILE* f = fopen(path, "rb");
    if(!f){ perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(n <= 0 || n > (1 << 20)){ fprintf(stderr, "%s: bad size\n", path); exit(2); }
    uint8_t* b = malloc((size_t)n);
    if(fread(b, 1, (size_t)n, f) != (size_t)n){ perror(path); exit(2); }
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

/* ---------------------------------------------------------------- fuzz */
static volatile uint64_t g_cur_seed;
static void on_alarm(int s){
    (void)s;
    fprintf(stderr, "HANG at seed %llu\n", (unsigned long long)g_cur_seed);
    _exit(3);
}
static void on_death(void){ fprintf(stderr, "CRASH at seed %llu\n", (unsigned long long)g_cur_seed); }
extern void __sanitizer_set_death_callback(void (*cb)(void));

static void fix_checksum(uint8_t* t, uint32_t len){
    if(len < 10) return;
    uint8_t s = 0;
    t[9] = 0;
    for(uint32_t i = 0; i < len; i++) s = (uint8_t)(s + t[i]);
    t[9] = (uint8_t)-s;
}

static void mutate(uint8_t* t, uint32_t len){
    int n = 1 + (int)(rnd() % 8);
    for(int k = 0; k < n && len; k++){
        uint32_t o = rnd() % len;
        switch(rnd() % 9){
            case 0: t[o] ^= (uint8_t)(1u << (rnd() % 8)); break;
            case 1: t[o] = (uint8_t)rnd(); break;
            case 2: t[o] = (rnd() & 1) ? 0xFF : 0x00; break;
            case 3: if(o + 1 < len){ uint16_t v = (uint16_t)((rnd() & 1) ? rnd() : rnd() % 64);
                    t[o] = (uint8_t)v; t[o + 1] = (uint8_t)(v >> 8); } break;
            case 4: if(o + 3 < len){ uint32_t v = rnd(); memcpy(t + o, &v, 4); } break;
            case 5: { uint32_t m = 1 + rnd() % 16; for(uint32_t i = 0; i < m && o + i < len; i++) t[o + i] = (uint8_t)rnd(); } break;
            case 6: { uint32_t a = rnd() % len, m = 1 + rnd() % 32;
                      for(uint32_t i = 0; i < m && o + i < len && a + i < len; i++) t[o + i] = t[a + i]; } break;
            case 7: if(len > 8){ uint32_t v = (rnd() & 1) ? len + rnd() % 64 : rnd() % len; memcpy(t + 4, &v, 4); } break;
            default: t[o] = (uint8_t)(t[o] + 1 - (rnd() % 3)); break;
        }
    }
}

static void exercise(void){
    /* Lookups on whatever acpi_dmar_init() left behind. */
    for(int i = 0; i < 64; i++){
        uint16_t bdf = (uint16_t)rnd(), seg = (uint16_t)(rnd() % 3), al;
        uint32_t first;
        (void)acpi_dmar_drhd_for(seg, bdf);
        (void)acpi_dmar_rmrr_covers(seg, bdf, &first);
        (void)acpi_ivrs_ivhd_for(seg, bdf, &al);
    }
    for(int id = 0; id < 8; id++){
        uint16_t x;
        (void)acpi_dmar_ioapic_sid((uint8_t)id, &x);
        (void)acpi_dmar_hpet_sid((uint8_t)id, &x);
        (void)acpi_ivrs_special_devid(1, (uint8_t)id, &x);
        (void)acpi_ivrs_special_devid(2, (uint8_t)id, &x);
    }
    /* Consistency: every index a consumer would follow stays in range. */
    const acpi_dmar_t* d = acpi_dmar_get();
    for(uint32_t i = 0; i < d->drhd_count; i++)
        if(d->drhd[i].scope_first + d->drhd[i].scope_count > d->scope_count){ fprintf(stderr, "drhd scope range\n"); abort(); }
    for(uint32_t i = 0; i < d->rmrr_count; i++)
        if(d->rmrr[i].scope_first + d->rmrr[i].scope_count > d->scope_count){ fprintf(stderr, "rmrr scope range\n"); abort(); }
    if(d->drhd_count > DMAR_MAX_DRHD || d->rmrr_count > DMAR_MAX_RMRR || d->scope_count > DMAR_MAX_SCOPES){ fprintf(stderr, "dmar count\n"); abort(); }
    const acpi_ivrs_t* v = acpi_ivrs_get();
    if(v->ivhd_count > IVRS_MAX_IVHD || v->ivmd_count > IVRS_MAX_IVMD || v->dev_count > IVRS_MAX_DEVS){ fprintf(stderr, "ivrs count\n"); abort(); }
    for(uint32_t i = 0; i < v->ivhd_count; i++)
        if(v->ivhd[i].dev_first + v->ivhd[i].dev_count > v->dev_count){ fprintf(stderr, "ivhd dev range\n"); abort(); }
    for(uint32_t i = 0; i < d->scope_count; i++)
        if(d->scopes[i].path_len < 1 || d->scopes[i].path_len > DMAR_MAX_PATH){ fprintf(stderr, "path len\n"); abort(); }
}

static int do_fuzz(long iters, uint64_t seed, int nseeds, char** files){
    uint8_t** seeds = calloc((size_t)nseeds, sizeof(*seeds));
    uint32_t* slen = calloc((size_t)nseeds, sizeof(*slen));
    for(int i = 0; i < nseeds; i++) seeds[i] = load(files[i], &slen[i]);
    signal(SIGALRM, on_alarm);
    __sanitizer_set_death_callback(on_death);
    uint64_t totreads = 0;
    long parsed = 0, present = 0;
    for(long it = 0; it < iters; it++){
        g_cur_seed = seed + (uint64_t)it;
        g_rng = g_cur_seed * 0x9E3779B97F4A7C15ull + 1;
        for(int w = 0; w < 4; w++) rnd();
        alarm(2);
        int s = (int)(rnd() % (uint32_t)nseeds);
        uint32_t len = slen[s];
        switch(rnd() % 6){
            case 0: if(len > 1) len = 1 + rnd() % len; break;               /* truncate */
            case 1: len += rnd() % 128; break;                              /* extend   */
            default: break;
        }
        if(rnd() % 50 == 0) len = rnd() % 64;                             /* tiny     */
        uint8_t* t = malloc(len ? len : 1);
        uint32_t keep = len < slen[s] ? len : slen[s];
        memcpy(t, seeds[s], keep);
        for(uint32_t i = keep; i < len; i++) t[i] = (uint8_t)rnd();
        if(rnd() % 4 == 0 && nseeds > 1){                                 /* splice   */
            int s2 = (int)(rnd() % (uint32_t)nseeds);
            if(slen[s2] > 48 && len > 48){
                uint32_t a = 48 + rnd() % (slen[s2] - 48), o = 48 + rnd() % (len - 48);
                for(uint32_t i = 0; a + i < slen[s2] && o + i < len && i < 64; i++) t[o + i] = seeds[s2][a + i];
            }
        }
        mutate(t, len);
        if(rnd() % 3 == 0 && len >= 8){                                   /* coherent length */
            uint32_t L = len; memcpy(t + 4, &L, 4);
        }
        if(rnd() & 1) fix_checksum(t, len);
        random_topology();

        /* Direct parser calls on private outputs ... */
        static acpi_dmar_t dm; static acpi_ivrs_t iv;
        if(acpi_dmar_parse(t, len, &dm) == 0) parsed++;
        if(acpi_ivrs_parse(t, len, &iv) == 0) parsed++;
        /* ... and the boot path (which fills the globals the lookups use). */
        g_tbl = t; g_tbl_len = len;
        acpi_dmar_init();
        if(acpi_dmar_get()->present || acpi_ivrs_get()->present) present++;
        exercise();
        totreads += g_reads; g_reads = 0;
        free(t);
        alarm(0);
        if((it + 1) % 10000 == 0) fprintf(stderr, "[acpi_fuzz] %ld/%ld\n", it + 1, iters);
    }
    printf("[acpi_fuzz] %ld iterations clean (seed %llu, %d seeds, %ld parsed, %ld via init, %llu cfg reads)\n",
           iters, (unsigned long long)seed, nseeds, parsed, present, (unsigned long long)totreads);
    for(int i = 0; i < nseeds; i++) free(seeds[i]);
    free(seeds); free(slen);
    return 0;
}

int main(int argc, char** argv){
    if(argc == 3 && !strcmp(argv[1], "dump")){
        uint32_t len;
        uint8_t* t = load(argv[2], &len);
        g_fuzz_verbose = 1;
        fixed_topology();
        g_tbl = t; g_tbl_len = len;
        acpi_dmar_init();
        fflush(stderr);
        dump_dmar();
        dump_ivrs();
        exercise();
        free(t);
        return 0;
    }
    if(argc >= 5 && !strcmp(argv[1], "fuzz")){
        if(getenv("ACPI_FUZZ_VERBOSE")) g_fuzz_verbose = 1;
        return do_fuzz(strtol(argv[2], NULL, 0), strtoull(argv[3], NULL, 0), argc - 4, argv + 4);
    }
    fprintf(stderr, "usage: %s dump TABLE | fuzz ITER SEED TABLE...\n", argv[0]);
    return 2;
}
