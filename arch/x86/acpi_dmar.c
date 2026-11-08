/*
 * SecOS Kernel - ACPI DMAR (Intel VT-d) and IVRS (AMD-Vi) parsing (M53b)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * See acpi_dmar.h. Sources: Intel VT-d spec (rev 4.x) chapter 8 "BIOS
 * Considerations" for DMAR; AMD I/O Virtualization Technology spec (rev 3.x)
 * section 5 "IVRS" for IVRS. No code taken from other kernels.
 *
 * Rules for every walk below:
 *  - an offset is advanced only by a length that was checked to be >= the
 *    minimum for its kind and to fit in what is left of its parent;
 *  - a length that fails those checks stops that walk (`truncated`), since
 *    nothing after it can be located;
 *  - a structure with a sane length but bad contents is skipped (`invalid`);
 *  - all arithmetic is done in uint64_t on values bounded by the table size;
 *  - every loop also has an iteration guard. Offsets strictly increase, so
 *    the guards (set above the most iterations a legal table can need) are a
 *    second line of defence, never a limit on real tables.
 *
 * This file is built unchanged by tools/acpi_fuzz (host, ASan+UBSan): it uses
 * only acpi_table_find() (acpi.c), acpi_dmar_cfg_read32() and debugcon.
 */
#include "acpi_dmar.h"
#include "acpi.h"
#include "debugcon.h"

static acpi_dmar_t g_dmar;
static acpi_ivrs_t g_ivrs;

/* ------------------------------------------------------------ helpers */
static uint16_t rd16(const uint8_t* p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t* p){ return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static void dm_zero(void* p, uint64_t n){ uint8_t* b = (uint8_t*)p; for(uint64_t i = 0; i < n; i++) b[i] = 0; }
static int  dm_sig(const uint8_t* p, const char* s){
    for(int i = 0; i < 4; i++) if(p[i] != (uint8_t)s[i]) return 0;
    return 1;
}
static int  dm_checksum_ok(const uint8_t* p, uint32_t len){
    uint8_t s = 0;
    for(uint32_t i = 0; i < len; i++) s = (uint8_t)(s + p[i]);
    return s == 0;
}

/* Small log formatter: everything goes out as whole lines. */
typedef struct { char b[160]; uint32_t n; } dm_line_t;
static void ln_s(dm_line_t* l, const char* s){ while(*s && l->n + 1 < sizeof(l->b)) l->b[l->n++] = *s++; }
static void ln_hex(dm_line_t* l, uint64_t v){
    char t[17]; int i = 16; t[i] = 0;
    do { t[--i] = "0123456789abcdef"[v & 0xF]; v >>= 4; } while(v && i > 0);
    ln_s(l, "0x"); ln_s(l, t + i);
}
static void ln_dec(dm_line_t* l, uint64_t v){
    char t[21]; int i = 20; t[i] = 0;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while(v && i > 0);
    ln_s(l, t + i);
}
static void ln_hx2(dm_line_t* l, uint32_t v, int digits){
    char t[5]; int i;
    for(i = digits - 1; i >= 0; i--){ t[i] = "0123456789abcdef"[v & 0xF]; v >>= 4; }
    t[digits] = 0; ln_s(l, t);
}
static void ln_bdf(dm_line_t* l, uint16_t bdf){
    ln_hx2(l, bdf >> 8, 2); ln_s(l, ":"); ln_hx2(l, (bdf >> 3) & 0x1F, 2);
    ln_s(l, "."); ln_hx2(l, bdf & 7, 1);
}
static void ln_end(dm_line_t* l){ l->b[l->n] = 0; debugcon_writestring(l->b); debugcon_writestring("\n"); l->n = 0; }
static void ln_begin(dm_line_t* l){ l->n = 0; ln_s(l, "[IOMMU] "); }

/* ------------------------------------------------- DMAR scope parsing */
#define DMAR_HDR_LEN   48
#define CFG_ABSENT     0xFFFFFFFFu

/* Resolve one scope's (start_bus, path) to a BDF through the bridges it
 * crosses (VT-d spec 8.3.1). Only reads config space: never writes. */
static void dmar_resolve(uint16_t seg, dmar_scope_t* s){
    uint8_t bus = s->start_bus;
    s->resolved = 0;
    for(uint32_t i = 0; i + 1 < s->path_len; i++){
        uint8_t d = s->path[i][0], f = s->path[i][1];
        uint32_t id = acpi_dmar_cfg_read32(seg, bus, d, f, 0x00);
        if(id == CFG_ABSENT || (id & 0xFFFF) == 0xFFFF) return;
        uint32_t hdr = acpi_dmar_cfg_read32(seg, bus, d, f, 0x0C);
        if(((hdr >> 16) & 0x7F) != 1) return;               /* not a PCI-PCI bridge */
        uint32_t buses = acpi_dmar_cfg_read32(seg, bus, d, f, 0x18);
        uint8_t sec = (uint8_t)(buses >> 8);
        if(sec <= bus) return;                                /* bridge not set up / loop */
        bus = sec;
    }
    uint8_t d = s->path[s->path_len - 1][0], f = s->path[s->path_len - 1][1];
    s->bdf = DMAR_BDF(bus, d, f);
    if(s->type == DMAR_SCOPE_ENDPOINT || s->type == DMAR_SCOPE_BRIDGE){
        uint32_t id = acpi_dmar_cfg_read32(seg, bus, d, f, 0x00);
        if(id == CFG_ABSENT || (id & 0xFFFF) == 0xFFFF) return;
        uint32_t hdr = acpi_dmar_cfg_read32(seg, bus, d, f, 0x0C);
        uint32_t hdr_type = (hdr >> 16) & 0x7F;
        if(s->type == DMAR_SCOPE_BRIDGE){
            if(hdr_type != 1) return;
            uint32_t buses = acpi_dmar_cfg_read32(seg, bus, d, f, 0x18);
            uint8_t sec = (uint8_t)(buses >> 8), sub = (uint8_t)(buses >> 16);
            if(sec <= bus || sub < sec) return;
            s->sec_bus = sec; s->sub_bus = sub;
        } else {
            uint32_t cls = acpi_dmar_cfg_read32(seg, bus, d, f, 0x08);
            if(hdr_type == 0 && (cls >> 24) == 0x03) s->is_display = 1;
        }
    }
    s->resolved = 1;
}

/* Parse the device scopes in [p, p+len) of one remapping structure into the
 * pool. Returns the number stored; *bad is set if anything was rejected. */
static uint16_t dmar_scopes(acpi_dmar_t* o, uint16_t seg, const uint8_t* p, uint32_t len,
                            uint16_t* first, uint8_t* bad){
    uint32_t off = 0;
    uint16_t n = 0;
    *first = (uint16_t)o->scope_count;
    for(uint32_t guard = 0; off < len && guard < (1u << 14); guard++){
        if(len - off < 2){ o->invalid++; *bad = 1; break; }
        uint8_t type = p[off], slen = p[off + 1];
        if(slen < 6 || slen > len - off){ o->invalid++; *bad = 1; break; }
        const uint8_t* e = p + off;
        off += slen;
        uint32_t hops = (uint32_t)(slen - 6) / 2;
        if(type < DMAR_SCOPE_ENDPOINT || type > DMAR_SCOPE_NAMESPACE ||
           (slen - 6) % 2 || hops == 0 || hops > DMAR_MAX_PATH){
            o->invalid++; *bad = 1; continue;
        }
        int ok = 1;
        for(uint32_t h = 0; h < hops; h++)
            if(e[6 + 2 * h] > 31 || e[7 + 2 * h] > 7) ok = 0;
        if(!ok){ o->invalid++; *bad = 1; continue; }
        if(o->scope_count >= DMAR_MAX_SCOPES){ o->dropped++; *bad = 1; continue; }
        dmar_scope_t* s = &o->scopes[o->scope_count];
        dm_zero(s, sizeof(*s));
        s->type = type;
        s->enum_id = e[4];
        s->start_bus = e[5];
        s->path_len = (uint8_t)hops;
        for(uint32_t h = 0; h < hops; h++){ s->path[h][0] = e[6 + 2 * h]; s->path[h][1] = e[7 + 2 * h]; }
        dmar_resolve(seg, s);
        o->scope_count++;
        n++;
    }
    if(off < len && *bad == 0){ o->invalid++; *bad = 1; }   /* guard hit */
    return n;
}

static void dmar_drhd(acpi_dmar_t* o, const uint8_t* e, uint32_t len){
    if(len < 16){ o->invalid++; return; }
    uint8_t  flags = e[4];
    uint8_t  sz = e[5] & 0x0F;
    uint16_t seg = rd16(e + 6);
    uint64_t base = rd64(e + 8);
    uint64_t span = 4096ull << sz;
    if(base == 0 || (base & 0xFFF) || base > ~0ull - span){ o->invalid++; return; }
    if(flags & DRHD_F_INCLUDE_PCI_ALL){
        for(uint32_t i = 0; i < o->drhd_count; i++)
            if(o->drhd[i].valid && o->drhd[i].include_all && o->drhd[i].segment == seg){
                o->invalid++; return;                         /* second catch-all */
            }
    }
    for(uint32_t i = 0; i < o->drhd_count; i++)
        if(o->drhd[i].valid && o->drhd[i].reg_base == base){ o->invalid++; return; }
    if(o->drhd_count >= DMAR_MAX_DRHD){ o->dropped++; return; }
    dmar_drhd_t* u = &o->drhd[o->drhd_count];
    dm_zero(u, sizeof(*u));
    u->flags = flags;
    u->include_all = (flags & DRHD_F_INCLUDE_PCI_ALL) ? 1 : 0;
    u->size_pages_log2 = sz;
    u->segment = seg;
    u->reg_base = base;
    u->scope_count = dmar_scopes(o, seg, e + 16, len - 16, &u->scope_first, &u->scopes_incomplete);
    /* Graphics-only: no catch-all, no bridge, at least one present display
     * endpoint and every present device a display endpoint. */
    if(!u->include_all){
        uint32_t disp = 0, other = 0;
        for(uint32_t i = 0; i < u->scope_count; i++){
            const dmar_scope_t* s = &o->scopes[u->scope_first + i];
            if(s->type == DMAR_SCOPE_BRIDGE) other++;
            else if(s->type == DMAR_SCOPE_ENDPOINT && s->resolved){
                if(s->is_display) disp++; else other++;
            }
        }
        u->gfx_only = (disp > 0 && other == 0) ? 1 : 0;
    }
    u->valid = 1;
    o->drhd_count++;
}

static void dmar_rmrr(acpi_dmar_t* o, const uint8_t* e, uint32_t len){
    if(len < 24){ o->invalid++; return; }
    uint16_t seg = rd16(e + 6);
    uint64_t base = rd64(e + 8), limit = rd64(e + 16);
    if((base & 0xFFF) || limit < base || limit == ~0ull || ((limit + 1) & 0xFFF)){
        o->invalid++; return;
    }
    if(o->rmrr_count >= DMAR_MAX_RMRR){ o->dropped++; return; }
    dmar_rmrr_t* r = &o->rmrr[o->rmrr_count];
    dm_zero(r, sizeof(*r));
    r->segment = seg; r->base = base; r->limit = limit;
    r->scope_count = dmar_scopes(o, seg, e + 24, len - 24, &r->scope_first, &r->scopes_incomplete);
    r->valid = 1;
    o->rmrr_count++;
}

/* ATSR and SATC share a layout: flags @4, segment @6, scopes @8. */
static void dmar_atsr_satc(acpi_dmar_t* o, const uint8_t* e, uint32_t len, int is_satc){
    if(len < 8){ o->invalid++; return; }
    uint8_t bad = 0;
    if(is_satc){
        if(o->satc_count >= DMAR_MAX_SATC){ o->dropped++; return; }
        dmar_satc_t* s = &o->satc[o->satc_count];
        dm_zero(s, sizeof(*s));
        s->flags = e[4]; s->segment = rd16(e + 6);
        s->scope_count = dmar_scopes(o, s->segment, e + 8, len - 8, &s->scope_first, &bad);
        s->valid = 1;
        o->satc_count++;
    } else {
        if(o->atsr_count >= DMAR_MAX_ATSR){ o->dropped++; return; }
        dmar_atsr_t* a = &o->atsr[o->atsr_count];
        dm_zero(a, sizeof(*a));
        a->flags = e[4]; a->segment = rd16(e + 6);
        a->scope_count = dmar_scopes(o, a->segment, e + 8, len - 8, &a->scope_first, &bad);
        a->valid = 1;
        o->atsr_count++;
    }
}

static void dmar_rhsa(acpi_dmar_t* o, const uint8_t* e, uint32_t len){
    if(len < 20){ o->invalid++; return; }
    uint64_t base = rd64(e + 8);
    if(base == 0 || (base & 0xFFF)){ o->invalid++; return; }
    if(o->rhsa_count >= DMAR_MAX_RHSA){ o->dropped++; return; }
    o->rhsa[o->rhsa_count].reg_base = base;
    o->rhsa[o->rhsa_count].proximity = rd32(e + 16);
    o->rhsa_count++;
}

int acpi_dmar_parse(const uint8_t* t, uint32_t len, acpi_dmar_t* o){
    dm_zero(o, sizeof(*o));
    if(!t || len < DMAR_HDR_LEN || !dm_sig(t, "DMAR")) return -1;
    uint32_t tlen = rd32(t + 4);
    if(tlen < DMAR_HDR_LEN) return -1;
    if(tlen > len){ tlen = len; o->truncated = 1; }
    o->present = 1;
    o->length = tlen;
    o->checksum_bad = dm_checksum_ok(t, tlen) ? 0 : 1;
    o->revision = t[8];
    o->haw = (uint16_t)(t[36] + 1);
    o->haw_bad = (o->haw < 32 || o->haw > 64) ? 1 : 0;
    o->flags = t[37];

    uint32_t off = DMAR_HDR_LEN;
    for(uint32_t guard = 0; off < tlen && guard < (1u << 18); guard++){
        if(tlen - off < 4){ o->truncated = 1; break; }
        uint16_t type = rd16(t + off), slen = rd16(t + off + 2);
        if(slen < 4 || slen > tlen - off){ o->truncated = 1; break; }
        const uint8_t* e = t + off;
        off += slen;
        switch(type){
            case 0: dmar_drhd(o, e, slen); break;
            case 1: dmar_rmrr(o, e, slen); break;
            case 2: dmar_atsr_satc(o, e, slen, 0); break;
            case 3: dmar_rhsa(o, e, slen); break;
            case 4: if(slen < 8) o->invalid++; else o->andd_count++; break;
            case 5: dmar_atsr_satc(o, e, slen, 1); break;
            default: o->unknown_count++; break;
        }
    }
    if(off < tlen && !o->truncated) o->truncated = 1;          /* guard hit */
    /* Attach RHSA proximity domains to their units. */
    for(uint32_t i = 0; i < o->rhsa_count; i++)
        for(uint32_t j = 0; j < o->drhd_count; j++)
            if(o->drhd[j].valid && o->drhd[j].reg_base == o->rhsa[i].reg_base){
                o->drhd[j].has_rhsa = 1;
                o->drhd[j].proximity = o->rhsa[i].proximity;
            }
    return 0;
}

/* ------------------------------------------------------- IVRS parsing */
#define IVRS_HDR_LEN   48

static int ivrs_add_dev(acpi_ivrs_t* o, ivrs_ivhd_t* h, const ivrs_dev_t* d){
    if(o->dev_count >= IVRS_MAX_DEVS){ o->dropped++; return -1; }
    o->devs[o->dev_count++] = *d;
    h->dev_count++;
    return 0;
}

/* Walk an IVHD's device entries in [p, p+len). */
static void ivrs_devs(acpi_ivrs_t* o, ivrs_ivhd_t* h, const uint8_t* p, uint32_t len){
    uint32_t off = 0;
    ivrs_dev_t pend;                 /* a pending start-of-range entry */
    int have_pend = 0;
    dm_zero(&pend, sizeof(pend));
    h->dev_first = (uint16_t)o->dev_count;
    for(uint32_t guard = 0; off < len && guard < (1u << 14); guard++){
        uint8_t type = p[off];
        uint32_t elen;
        if(type < 0x80) elen = 4u << (type >> 6);
        else if(type == 0xF0){
            if(len - off < 22){ o->invalid++; o->truncated = 1; return; }
            elen = 22u + p[off + 21];
        } else { o->invalid++; o->truncated = 1; return; }   /* length unknown */
        if(elen > len - off){ o->invalid++; o->truncated = 1; return; }
        const uint8_t* e = p + off;
        off += elen;

        if(have_pend && type != 0x04){ o->invalid++; have_pend = 0; } /* range never closed */
        ivrs_dev_t d;
        dm_zero(&d, sizeof(d));
        d.dte = e[3];
        d.start = d.end = rd16(e + 1);
        switch(type){
            case 0x00: break;                                       /* padding */
            case 0x01: d.kind = IVRS_DEV_ALL; d.start = 0; d.end = 0xFFFF; ivrs_add_dev(o, h, &d); break;
            case 0x02: d.kind = IVRS_DEV_SELECT; ivrs_add_dev(o, h, &d); break;
            case 0x03: d.kind = IVRS_DEV_RANGE; pend = d; have_pend = 1; break;
            case 0x42: d.kind = IVRS_DEV_ALIAS; d.alias = rd16(e + 5); ivrs_add_dev(o, h, &d); break;
            case 0x43: d.kind = IVRS_DEV_ALIAS; d.alias = rd16(e + 5); pend = d; have_pend = 1; break;
            case 0x46: d.kind = IVRS_DEV_EXT; d.ext = rd32(e + 4); ivrs_add_dev(o, h, &d); break;
            case 0x47: d.kind = IVRS_DEV_EXT; d.ext = rd32(e + 4); pend = d; have_pend = 1; break;
            case 0x04:
                if(!have_pend){ o->invalid++; break; }
                have_pend = 0;
                if(d.start < pend.start){ o->invalid++; break; }
                pend.end = d.start;
                ivrs_add_dev(o, h, &pend);
                break;
            case 0x48:
                d.kind = IVRS_DEV_SPECIAL;
                d.handle = e[4];
                d.start = d.end = rd16(e + 5);
                d.variety = e[7];
                if(d.variety != IVRS_SPECIAL_IOAPIC && d.variety != IVRS_SPECIAL_HPET){ o->invalid++; break; }
                ivrs_add_dev(o, h, &d);
                break;
            case 0xF0:
                if(h->type == 0x10){ o->invalid++; break; }         /* 11h/40h only */
                d.kind = IVRS_DEV_ACPI_HID;
                ivrs_add_dev(o, h, &d);
                break;
            default: o->unknown_count++; break;                     /* reserved type */
        }
    }
    if(have_pend) o->invalid++;
    if(off < len) o->truncated = 1;                                 /* guard hit */
}

static void ivrs_ivhd(acpi_ivrs_t* o, const uint8_t* e, uint32_t len, uint8_t type){
    uint32_t hl = (type == 0x10) ? 24 : 40;
    if(len < hl){ o->invalid++; return; }
    uint64_t base = rd64(e + 8);
    if(base == 0 || (base & 0x3FFF)){ o->invalid++; return; }
    if(o->ivhd_count >= IVRS_MAX_IVHD){ o->dropped++; return; }
    ivrs_ivhd_t* h = &o->ivhd[o->ivhd_count];
    dm_zero(h, sizeof(*h));
    h->type = type;
    h->flags = e[1];
    h->devid = rd16(e + 4);
    h->cap_offset = rd16(e + 6);
    h->base = base;
    h->segment = rd16(e + 16);
    h->info = rd16(e + 18);
    h->attr = rd32(e + 20);
    if(type != 0x10){ h->efr = rd64(e + 24); h->efr2 = rd64(e + 32); }
    ivrs_devs(o, h, e + hl, len - hl);
    h->valid = 1;
    o->ivhd_count++;
}

static void ivrs_ivmd(acpi_ivrs_t* o, const uint8_t* e, uint32_t len, uint8_t type){
    if(len < 32){ o->invalid++; return; }
    uint16_t start = rd16(e + 4), aux = rd16(e + 6);
    uint64_t base = rd64(e + 16), blen = rd64(e + 24);
    if((base & 0xFFF) || blen == 0 || base > ~0ull - blen){ o->invalid++; return; }
    if(type == 0x22 && aux < start){ o->invalid++; return; }
    if(o->ivmd_count >= IVRS_MAX_IVMD){ o->dropped++; return; }
    ivrs_ivmd_t* m = &o->ivmd[o->ivmd_count++];
    dm_zero(m, sizeof(*m));
    m->type = type;
    m->flags = e[1];
    m->start = (type == 0x20) ? 0 : start;
    m->end = (type == 0x20) ? 0xFFFF : (type == 0x22 ? aux : start);
    m->base = base;
    m->length = blen;
    m->valid = 1;
}

int acpi_ivrs_parse(const uint8_t* t, uint32_t len, acpi_ivrs_t* o){
    dm_zero(o, sizeof(*o));
    if(!t || len < IVRS_HDR_LEN || !dm_sig(t, "IVRS")) return -1;
    uint32_t tlen = rd32(t + 4);
    if(tlen < IVRS_HDR_LEN) return -1;
    if(tlen > len){ tlen = len; o->truncated = 1; }
    o->present = 1;
    o->length = tlen;
    o->checksum_bad = dm_checksum_ok(t, tlen) ? 0 : 1;
    o->revision = t[8];
    o->ivinfo = rd32(t + 36);

    uint32_t off = IVRS_HDR_LEN;
    for(uint32_t guard = 0; off < tlen && guard < (1u << 18); guard++){
        if(tlen - off < 4){ o->truncated = 1; break; }
        uint8_t type = t[off];
        uint16_t slen = rd16(t + off + 2);
        if(slen < 4 || slen > tlen - off){ o->truncated = 1; break; }
        const uint8_t* e = t + off;
        off += slen;
        switch(type){
            case 0x10: case 0x11: case 0x40: ivrs_ivhd(o, e, slen, type); break;
            case 0x20: case 0x21: case 0x22: ivrs_ivmd(o, e, slen, type); break;
            default: o->unknown_count++; break;
        }
    }
    if(off < tlen && !o->truncated) o->truncated = 1;
    /* An OS uses the IVHD blocks of the highest type it supports (all three
     * here); lower-type blocks describe the same IOMMUs for older OSes. */
    for(uint32_t i = 0; i < o->ivhd_count; i++)
        if(o->ivhd[i].valid && o->ivhd[i].type > o->primary_type) o->primary_type = o->ivhd[i].type;
    for(uint32_t i = 0; i < o->ivhd_count; i++)
        o->ivhd[i].primary = (o->ivhd[i].valid && o->ivhd[i].type == o->primary_type) ? 1 : 0;
    return 0;
}

/* ------------------------------------------------------------ logging */
static const char* scope_name(uint8_t t){
    switch(t){
        case DMAR_SCOPE_ENDPOINT: return "dev";
        case DMAR_SCOPE_BRIDGE:   return "bridge";
        case DMAR_SCOPE_IOAPIC:   return "ioapic";
        case DMAR_SCOPE_HPET:     return "hpet";
        default:                  return "acpi";
    }
}

/* Scope list of one structure, at most 6 per line, at most 4 lines. */
static void log_scopes(const acpi_dmar_t* d, uint16_t first, uint16_t count){
    dm_line_t l;
    uint32_t shown = 0;
    while(shown < count && shown < 24){
        ln_begin(&l); ln_s(&l, "  ");
        for(uint32_t k = 0; k < 6 && shown < count && shown < 24; k++, shown++){
            const dmar_scope_t* s = &d->scopes[first + shown];
            ln_s(&l, " "); ln_s(&l, scope_name(s->type));
            if(s->type >= DMAR_SCOPE_IOAPIC){ ln_s(&l, "#"); ln_dec(&l, s->enum_id); }
            ln_s(&l, "="); ln_bdf(&l, s->bdf);
            if(s->type == DMAR_SCOPE_BRIDGE && s->resolved){
                ln_s(&l, "[bus "); ln_hx2(&l, s->sec_bus, 2); ln_s(&l, "-"); ln_hx2(&l, s->sub_bus, 2); ln_s(&l, "]");
            }
            if(!s->resolved) ln_s(&l, "(absent)");
            else if(s->is_display) ln_s(&l, "(display)");
        }
        ln_end(&l);
    }
    if(count > shown){ ln_begin(&l); ln_s(&l, "   ... "); ln_dec(&l, count - shown); ln_s(&l, " more"); ln_end(&l); }
}

void acpi_dmar_log(const acpi_dmar_t* d){
    dm_line_t l;
    if(!d->present) return;
    ln_begin(&l);
    ln_s(&l, "DMAR: "); ln_dec(&l, d->drhd_count); ln_s(&l, " DRHD haw="); ln_dec(&l, d->haw);
    ln_s(&l, " flags="); ln_hex(&l, d->flags);
    if(d->flags & DMAR_F_INTR_REMAP) ln_s(&l, " INTR_REMAP");
    if(d->flags & DMAR_F_X2APIC_OPT_OUT) ln_s(&l, " X2APIC_OPT_OUT");
    if(d->flags & DMAR_F_DMA_CTRL_OPT_IN) ln_s(&l, " DMA_CTRL_PLATFORM_OPT_IN");
    ln_s(&l, " rmrr="); ln_dec(&l, d->rmrr_count);
    ln_s(&l, " atsr="); ln_dec(&l, d->atsr_count);
    ln_s(&l, " rhsa="); ln_dec(&l, d->rhsa_count);
    ln_s(&l, " satc="); ln_dec(&l, d->satc_count);
    if(d->andd_count){ ln_s(&l, " andd="); ln_dec(&l, d->andd_count); }
    ln_end(&l);
    for(uint32_t i = 0; i < d->drhd_count; i++){
        const dmar_drhd_t* u = &d->drhd[i];
        ln_begin(&l);
        ln_s(&l, "DRHD "); ln_dec(&l, i); ln_s(&l, ": base="); ln_hex(&l, u->reg_base);
        ln_s(&l, " seg="); ln_dec(&l, u->segment);
        if(u->include_all) ln_s(&l, " INCLUDE_PCI_ALL");
        if(u->gfx_only) ln_s(&l, " graphics-only");
        ln_s(&l, " scopes="); ln_dec(&l, u->scope_count);
        if(u->has_rhsa){ ln_s(&l, " node="); ln_dec(&l, u->proximity); }
        if(u->scopes_incomplete) ln_s(&l, " (scopes incomplete)");
        ln_end(&l);
        log_scopes(d, u->scope_first, u->scope_count);
    }
    for(uint32_t i = 0; i < d->rmrr_count; i++){
        const dmar_rmrr_t* r = &d->rmrr[i];
        ln_begin(&l);
        ln_s(&l, "RMRR "); ln_dec(&l, i); ln_s(&l, ": "); ln_hex(&l, r->base);
        ln_s(&l, "-"); ln_hex(&l, r->limit); ln_s(&l, " seg="); ln_dec(&l, r->segment);
        ln_s(&l, " scopes="); ln_dec(&l, r->scope_count);
        if(r->scopes_incomplete) ln_s(&l, " (scopes incomplete)");
        ln_end(&l);
        log_scopes(d, r->scope_first, r->scope_count);
    }
    for(uint32_t i = 0; i < d->atsr_count; i++){
        ln_begin(&l); ln_s(&l, "ATSR "); ln_dec(&l, i); ln_s(&l, ": seg="); ln_dec(&l, d->atsr[i].segment);
        if(d->atsr[i].flags & ATSR_F_ALL_PORTS) ln_s(&l, " ALL_PORTS");
        ln_s(&l, " scopes="); ln_dec(&l, d->atsr[i].scope_count); ln_end(&l);
    }
    for(uint32_t i = 0; i < d->satc_count; i++){
        ln_begin(&l); ln_s(&l, "SATC "); ln_dec(&l, i); ln_s(&l, ": seg="); ln_dec(&l, d->satc[i].segment);
        if(d->satc[i].flags & SATC_F_ATC_REQUIRED) ln_s(&l, " ATC_REQUIRED");
        ln_s(&l, " scopes="); ln_dec(&l, d->satc[i].scope_count); ln_end(&l);
    }
    if(d->checksum_bad || d->haw_bad || d->invalid || d->dropped || d->truncated || d->unknown_count){
        ln_begin(&l); ln_s(&l, "DMAR WARN:");
        if(d->checksum_bad) ln_s(&l, " bad checksum");
        if(d->haw_bad) ln_s(&l, " bad haw");
        if(d->truncated) ln_s(&l, " truncated");
        ln_s(&l, " invalid="); ln_dec(&l, d->invalid);
        ln_s(&l, " dropped="); ln_dec(&l, d->dropped);
        ln_s(&l, " unknown="); ln_dec(&l, d->unknown_count);
        ln_end(&l);
    }
}

static const char* ivrs_kind_name(uint8_t k){
    switch(k){
        case IVRS_DEV_ALL:      return "all";
        case IVRS_DEV_SELECT:   return "dev";
        case IVRS_DEV_RANGE:    return "range";
        case IVRS_DEV_ALIAS:    return "alias";
        case IVRS_DEV_EXT:      return "ext";
        case IVRS_DEV_SPECIAL:  return "special";
        default:                return "acpihid";
    }
}

void acpi_ivrs_log(const acpi_ivrs_t* v){
    dm_line_t l;
    if(!v->present) return;
    ln_begin(&l);
    ln_s(&l, "IVRS: "); ln_dec(&l, v->ivhd_count); ln_s(&l, " IVHD primary-type="); ln_hex(&l, v->primary_type);
    ln_s(&l, " ivinfo="); ln_hex(&l, v->ivinfo);
    ln_s(&l, " va="); ln_dec(&l, (v->ivinfo >> 15) & 0x7F);
    ln_s(&l, " pa="); ln_dec(&l, (v->ivinfo >> 8) & 0x7F);
    ln_s(&l, " ivmd="); ln_dec(&l, v->ivmd_count);
    ln_end(&l);
    for(uint32_t i = 0; i < v->ivhd_count; i++){
        const ivrs_ivhd_t* h = &v->ivhd[i];
        ln_begin(&l);
        ln_s(&l, "IVHD "); ln_dec(&l, i); ln_s(&l, ": type="); ln_hex(&l, h->type);
        ln_s(&l, h->primary ? " (used)" : " (legacy)");
        ln_s(&l, " iommu="); ln_bdf(&l, h->devid); ln_s(&l, " base="); ln_hex(&l, h->base);
        ln_s(&l, " seg="); ln_dec(&l, h->segment); ln_s(&l, " flags="); ln_hex(&l, h->flags);
        ln_s(&l, " entries="); ln_dec(&l, h->dev_count);
        ln_end(&l);
        if(!h->primary) continue;
        uint32_t shown = 0;
        while(shown < h->dev_count && shown < 24){
            ln_begin(&l); ln_s(&l, "  ");
            for(uint32_t k = 0; k < 5 && shown < h->dev_count && shown < 24; k++, shown++){
                const ivrs_dev_t* d = &v->devs[h->dev_first + shown];
                ln_s(&l, " "); ln_s(&l, ivrs_kind_name(d->kind));
                if(d->kind == IVRS_DEV_SPECIAL){
                    ln_s(&l, d->variety == IVRS_SPECIAL_IOAPIC ? "-ioapic#" : "-hpet#");
                    ln_dec(&l, d->handle);
                }
                if(d->kind != IVRS_DEV_ALL){
                    ln_s(&l, "="); ln_bdf(&l, d->start);
                    if(d->end != d->start){ ln_s(&l, ".."); ln_bdf(&l, d->end); }
                }
                if(d->kind == IVRS_DEV_ALIAS){ ln_s(&l, "->"); ln_bdf(&l, d->alias); }
            }
            ln_end(&l);
        }
        if(h->dev_count > shown){ ln_begin(&l); ln_s(&l, "   ... "); ln_dec(&l, h->dev_count - shown); ln_s(&l, " more"); ln_end(&l); }
    }
    for(uint32_t i = 0; i < v->ivmd_count; i++){
        const ivrs_ivmd_t* m = &v->ivmd[i];
        ln_begin(&l);
        ln_s(&l, "IVMD "); ln_dec(&l, i); ln_s(&l, ": type="); ln_hex(&l, m->type);
        ln_s(&l, " "); ln_hex(&l, m->base); ln_s(&l, "+"); ln_hex(&l, m->length);
        ln_s(&l, " devs="); ln_bdf(&l, m->start); ln_s(&l, ".."); ln_bdf(&l, m->end);
        ln_s(&l, " flags="); ln_hex(&l, m->flags);
        ln_end(&l);
    }
    if(v->checksum_bad || v->invalid || v->dropped || v->truncated || v->unknown_count){
        ln_begin(&l); ln_s(&l, "IVRS WARN:");
        if(v->checksum_bad) ln_s(&l, " bad checksum");
        if(v->truncated) ln_s(&l, " truncated");
        ln_s(&l, " invalid="); ln_dec(&l, v->invalid);
        ln_s(&l, " dropped="); ln_dec(&l, v->dropped);
        ln_s(&l, " unknown="); ln_dec(&l, v->unknown_count);
        ln_end(&l);
    }
}

/* ----------------------------------------------------------- boot glue */
#define DMAR_TABLE_MAX  (1u << 20)   /* DMAR/IVRS bigger than 1 MiB: refused */

void acpi_dmar_init(void){
    uint32_t len = 0;
    dm_zero(&g_dmar, sizeof(g_dmar));
    dm_zero(&g_ivrs, sizeof(g_ivrs));
    const uint8_t* t = acpi_table_find("DMAR", &len);
    if(t && len <= DMAR_TABLE_MAX){
        if(acpi_dmar_parse(t, len, &g_dmar) == 0) acpi_dmar_log(&g_dmar);
        else debugcon_writestring("[IOMMU] DMAR present but unusable (bad header)\n");
    } else if(t) {
        debugcon_writestring("[IOMMU] DMAR refused: table larger than 1 MiB\n");
    }
    len = 0;
    t = acpi_table_find("IVRS", &len);
    if(t && len <= DMAR_TABLE_MAX){
        if(acpi_ivrs_parse(t, len, &g_ivrs) == 0) acpi_ivrs_log(&g_ivrs);
        else debugcon_writestring("[IOMMU] IVRS present but unusable (bad header)\n");
    } else if(t) {
        debugcon_writestring("[IOMMU] IVRS refused: table larger than 1 MiB\n");
    }
    if(!g_dmar.present && !g_ivrs.present) debugcon_writestring("[IOMMU] none\n");
}

const acpi_dmar_t* acpi_dmar_get(void){ return &g_dmar; }
const acpi_ivrs_t* acpi_ivrs_get(void){ return &g_ivrs; }

/* --------------------------------------------------------- lookups */
static int scope_matches(const dmar_scope_t* s, uint16_t bdf){
    if(!s->resolved) return 0;
    if(s->type == DMAR_SCOPE_ENDPOINT) return s->bdf == bdf;
    if(s->type == DMAR_SCOPE_BRIDGE){
        uint8_t bus = (uint8_t)(bdf >> 8);
        return s->bdf == bdf || (bus >= s->sec_bus && bus <= s->sub_bus);
    }
    return 0;
}

const dmar_drhd_t* acpi_dmar_drhd_for(uint16_t segment, uint16_t bdf){
    const acpi_dmar_t* d = &g_dmar;
    const dmar_drhd_t* all = 0;
    for(uint32_t i = 0; i < d->drhd_count; i++){
        const dmar_drhd_t* u = &d->drhd[i];
        if(!u->valid || u->segment != segment) continue;
        if(u->include_all){ if(!all) all = u; continue; }
        for(uint32_t k = 0; k < u->scope_count; k++)
            if(scope_matches(&d->scopes[u->scope_first + k], bdf)) return u;
    }
    return all;
}

static int dmar_special_sid(uint8_t type, uint8_t id, uint16_t* sid){
    const acpi_dmar_t* d = &g_dmar;
    for(uint32_t i = 0; i < d->drhd_count; i++){
        const dmar_drhd_t* u = &d->drhd[i];
        if(!u->valid) continue;
        for(uint32_t k = 0; k < u->scope_count; k++){
            const dmar_scope_t* s = &d->scopes[u->scope_first + k];
            if(s->type == type && s->enum_id == id && s->resolved){ if(sid) *sid = s->bdf; return 0; }
        }
    }
    return -1;
}
int acpi_dmar_ioapic_sid(uint8_t ioapic_id, uint16_t* sid){ return dmar_special_sid(DMAR_SCOPE_IOAPIC, ioapic_id, sid); }
int acpi_dmar_hpet_sid(uint8_t hpet_id, uint16_t* sid){ return dmar_special_sid(DMAR_SCOPE_HPET, hpet_id, sid); }

int acpi_dmar_rmrr_covers(uint16_t segment, uint16_t bdf, uint32_t* first){
    const acpi_dmar_t* d = &g_dmar;
    for(uint32_t i = 0; i < d->rmrr_count; i++){
        const dmar_rmrr_t* r = &d->rmrr[i];
        if(!r->valid || r->segment != segment) continue;
        for(uint32_t k = 0; k < r->scope_count; k++)
            if(scope_matches(&d->scopes[r->scope_first + k], bdf)){ if(first) *first = i; return 1; }
    }
    return 0;
}

const ivrs_ivhd_t* acpi_ivrs_ivhd_for(uint16_t segment, uint16_t devid, uint16_t* alias){
    const acpi_ivrs_t* v = &g_ivrs;
    for(uint32_t i = 0; i < v->ivhd_count; i++){
        const ivrs_ivhd_t* h = &v->ivhd[i];
        if(!h->valid || !h->primary || h->segment != segment) continue;
        for(uint32_t k = 0; k < h->dev_count; k++){
            const ivrs_dev_t* e = &v->devs[h->dev_first + k];
            if(e->kind == IVRS_DEV_SPECIAL || e->kind == IVRS_DEV_ACPI_HID) continue;
            if(devid < e->start || devid > e->end) continue;
            if(alias) *alias = (e->kind == IVRS_DEV_ALIAS) ? e->alias : devid;
            return h;
        }
    }
    return 0;
}

int acpi_ivrs_special_devid(uint8_t variety, uint8_t handle, uint16_t* devid){
    const acpi_ivrs_t* v = &g_ivrs;
    for(uint32_t i = 0; i < v->ivhd_count; i++){
        const ivrs_ivhd_t* h = &v->ivhd[i];
        if(!h->valid || !h->primary) continue;
        for(uint32_t k = 0; k < h->dev_count; k++){
            const ivrs_dev_t* e = &v->devs[h->dev_first + k];
            if(e->kind == IVRS_DEV_SPECIAL && e->variety == variety && e->handle == handle){
                if(devid) *devid = e->start;
                return 0;
            }
        }
    }
    return -1;
}
