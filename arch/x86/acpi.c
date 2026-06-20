/*
 * SecOS Kernel - ACPI table discovery (Phase I, M28-1)
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * See acpi.h. ACPI tables live in physical memory; we read them through the
 * physmap (phys_to_virt). All multi-byte fields are little-endian (native x86).
 */
#include "acpi.h"
#include "../../mm/vmm.h"
#include "debugcon.h"

static acpi_topology_t g_topo;

/* ACPI tables live in low RAM. Read them through the kernel's permanent low
 * identity map (0-512 MiB) — it is always present, whereas the physmap may not
 * yet cover the ACPI region on the UEFI path. Fall back to the physmap above 512 MiB. */
static const uint8_t* pv(uint64_t phys){
    if(phys < 0x20000000ULL) return (const uint8_t*)phys;
    return (const uint8_t*)phys_to_virt(phys);
}
static uint16_t rd16(const uint8_t* p){ return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t rd32(const uint8_t* p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint64_t rd64(const uint8_t* p){ return (uint64_t)rd32(p) | ((uint64_t)rd32(p+4)<<32); }

static int sig_eq(const uint8_t* p, const char* s, int n){ for(int i=0;i<n;i++) if(p[i]!=(uint8_t)s[i]) return 0; return 1; }
static int checksum_ok(const uint8_t* p, uint32_t len){ uint8_t s=0; for(uint32_t i=0;i<len;i++) s=(uint8_t)(s+p[i]); return s==0; }

/* Locate the RSDP: the 16-byte-aligned "RSD PTR " signature in the EBDA (first
 * 1 KiB) and the BIOS ROM area 0xE0000-0xFFFFF. Returns its physical address. */
static uint64_t find_rsdp(void){
    uint64_t ebda = (uint64_t)rd16(pv(0x40E)) << 4;        /* EBDA segment * 16   */
    if(ebda >= 0x400 && ebda < 0xA0000){
        for(uint64_t a=ebda; a<ebda+1024; a+=16)
            if(sig_eq(pv(a),"RSD PTR ",8) && checksum_ok(pv(a),20)) return a;
    }
    for(uint64_t a=0xE0000; a<0x100000; a+=16)
        if(sig_eq(pv(a),"RSD PTR ",8) && checksum_ok(pv(a),20)) return a;
    return 0;
}

/* Parse the MADT (APIC) at physical 'madt' into the topology. */
static void parse_madt(uint64_t madt){
    const uint8_t* t = pv(madt);
    uint32_t len = rd32(t+4);
    g_topo.lapic_base = rd32(t+36);                        /* 32-bit LAPIC addr   */
    uint32_t off = 44;                                     /* entries start at 44 */
    while(off + 2 <= len){
        uint8_t type = t[off], elen = t[off+1];
        if(elen < 2) break;
        const uint8_t* e = t+off;
        switch(type){
            case 0:                                        /* Processor Local APIC */
                if((rd32(e+4) & 1) && g_topo.cpu_count < ACPI_MAX_CPUS)   /* enabled */
                    g_topo.lapic_ids[g_topo.cpu_count++] = e[3];          /* apic_id */
                break;
            case 1:                                        /* IOAPIC */
                if(g_topo.ioapic_count < ACPI_MAX_IOAPICS){
                    g_topo.ioapics[g_topo.ioapic_count].id = e[2];
                    g_topo.ioapics[g_topo.ioapic_count].base = rd32(e+4);
                    g_topo.ioapics[g_topo.ioapic_count].gsi_base = rd32(e+8);
                    g_topo.ioapic_count++;
                }
                break;
            case 2:                                        /* Interrupt Source Override */
                if(g_topo.override_count < ACPI_MAX_OVERRIDES){
                    g_topo.overrides[g_topo.override_count].src_irq = e[3];
                    g_topo.overrides[g_topo.override_count].gsi = rd32(e+4);
                    g_topo.overrides[g_topo.override_count].flags = rd16(e+8);
                    g_topo.override_count++;
                }
                break;
            case 5:                                        /* LAPIC Address Override (64-bit) */
                g_topo.lapic_base = rd64(e+4);
                break;
            default: break;
        }
        off += elen;
    }
}

/* Walk the RSDT/XSDT pointer array and find a table by 4-char signature. */
static uint64_t find_table(uint64_t sdt, int is_xsdt, const char* sig){
    const uint8_t* h = pv(sdt);
    uint32_t len = rd32(h+4);
    uint32_t entsz = is_xsdt ? 8 : 4;
    uint32_t n = (len > 36) ? (len - 36) / entsz : 0;
    for(uint32_t i=0;i<n;i++){
        uint64_t tp = is_xsdt ? rd64(h+36+i*8) : rd32(h+36+i*4);
        if(sig_eq(pv(tp),sig,4)) return tp;
    }
    return 0;
}
static uint64_t find_madt(uint64_t sdt, int is_xsdt){ return find_table(sdt, is_xsdt, "APIC"); }

/* Parse the FADT ("FACP") for the PM1a/b control ports and the DSDT, then scan
 * the DSDT's AML for the \_S5 package to get the soft-off SLP_TYP values — the
 * standard portable ACPI shutdown recipe (works on QEMU, VMware, real HW). */
static void parse_fadt(uint64_t fadt){
    const uint8_t* f = pv(fadt);
    g_topo.pm1a_cnt = rd32(f + 64);     /* PM1a_CNT_BLK */
    g_topo.pm1b_cnt = rd32(f + 68);     /* PM1b_CNT_BLK */
    uint64_t dsdt = rd32(f + 40);       /* DSDT (32-bit) */
    if(rd32(f+4) >= 148){               /* FADT long enough for X_DSDT (64-bit) */
        uint64_t xdsdt = rd64(f + 140);
        if(xdsdt) dsdt = xdsdt;
    }
    if(!dsdt) return;
    const uint8_t* d = pv(dsdt);
    uint32_t dlen = rd32(d + 4);
    /* Find "_S5_" (5F 53 35 5F) and parse the following Package. */
    for(uint32_t i = 36; i + 6 < dlen; i++){
        if(d[i]=='_' && d[i+1]=='S' && d[i+2]=='5' && d[i+3]=='_'){
            const uint8_t* p = d + i + 4;
            /* Optional: NameOp(0x08) precedes the name; the value is a Package. */
            if(*p == 0x08) p++;          /* (rare positioning) */
            if(*p != 0x12) continue;     /* PackageOp expected */
            p++;                         /* skip PackageOp */
            p++;                         /* skip PkgLength (1 byte; small pkg) */
            p++;                         /* skip NumElements */
            /* element 0 -> SLP_TYPa, element 1 -> SLP_TYPb. Constants: 0x00=Zero,
             * 0x01=One, 0x0A xx = byte. */
            for(int e=0; e<2; e++){
                uint16_t v = 0;
                if(*p == 0x0A){ p++; v = *p++; }
                else if(*p == 0x00){ v = 0; p++; }
                else if(*p == 0x01){ v = 1; p++; }
                else { p++; }
                if(e==0) g_topo.slp_typa = v; else g_topo.slp_typb = v;
            }
            g_topo.s5_ok = (g_topo.pm1a_cnt != 0);
            return;
        }
    }
}

int acpi_init(uint64_t rsdp_hint){
    for(unsigned i=0;i<sizeof(g_topo);i++) ((uint8_t*)&g_topo)[i]=0;
    /* Prefer the RSDP handed over by the UEFI loader (the only reliable source on
     * UEFI); fall back to scanning the legacy BIOS area (MB2/SeaBIOS). */
    uint64_t rsdp = 0;
    if(rsdp_hint && sig_eq(pv(rsdp_hint),"RSD PTR ",8) && checksum_ok(pv(rsdp_hint),20)) rsdp = rsdp_hint;
    if(!rsdp) rsdp = find_rsdp();
    if(!rsdp){ debugcon_writestring("[ACPI] RSDP not found\n"); return -1; }
    const uint8_t* r = pv(rsdp);
    uint8_t rev = r[15];
    uint64_t madt = 0, sdt = 0; int is_xsdt = 0;
    if(rev >= 2){                                          /* ACPI 2.0+: prefer XSDT */
        uint64_t xsdt = rd64(r+24);
        if(xsdt && sig_eq(pv(xsdt),"XSDT",4)){ sdt = xsdt; is_xsdt = 1; madt = find_madt(xsdt, 1); }
    }
    if(!madt){                                             /* fall back to RSDT */
        uint64_t rsdt = rd32(r+16);
        if(rsdt && sig_eq(pv(rsdt),"RSDT",4)){ sdt = rsdt; is_xsdt = 0; madt = find_madt(rsdt, 0); }
    }
    /* FADT for ACPI poweroff (independent of the MADT). */
    if(sdt){ uint64_t fadt = find_table(sdt, is_xsdt, "FACP"); if(fadt) parse_fadt(fadt); }
    if(!madt){ debugcon_writestring("[ACPI] MADT not found\n"); return -1; }
    g_topo.lapic_base = 0xFEE00000;                        /* sane default before parse */
    parse_madt(madt);
    if(g_topo.cpu_count == 0){ g_topo.cpu_count = 1; g_topo.lapic_ids[0] = 0; }
    g_topo.bsp_lapic_id = g_topo.lapic_ids[0];
    g_topo.found = 1;

    debugcon_writestring("[ACPI] CPUs="); debugcon_print_hex(g_topo.cpu_count);
    debugcon_writestring(" lapic="); debugcon_print_hex(g_topo.lapic_base);
    debugcon_writestring(" ioapics="); debugcon_print_hex(g_topo.ioapic_count);
    if(g_topo.ioapic_count){ debugcon_writestring(" ioapic0="); debugcon_print_hex(g_topo.ioapics[0].base);
        debugcon_writestring(" gsi_base="); debugcon_print_hex(g_topo.ioapics[0].gsi_base); }
    debugcon_writestring(" overrides="); debugcon_print_hex(g_topo.override_count);
    debugcon_writestring("\n");
    return 0;
}

const acpi_topology_t* acpi_get(void){ return &g_topo; }

static inline void outw(uint16_t port, uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(port)); }

void acpi_poweroff(void){
    const uint16_t SLP_EN = 1u << 13;
    debugcon_writestring("[ACPI] poweroff: pm1a="); debugcon_print_hex(g_topo.pm1a_cnt);
    debugcon_writestring(" slp_typa="); debugcon_print_hex(g_topo.slp_typa);
    debugcon_writestring(" s5_ok="); debugcon_print_hex((uint64_t)g_topo.s5_ok);
    debugcon_writestring("\n");
    /* Preferred: the FADT/_S5 values (correct on real HW + VMware). */
    if(g_topo.s5_ok && g_topo.pm1a_cnt){
        outw((uint16_t)g_topo.pm1a_cnt, (uint16_t)((g_topo.slp_typa << 10) | SLP_EN));
        if(g_topo.pm1b_cnt)
            outw((uint16_t)g_topo.pm1b_cnt, (uint16_t)((g_topo.slp_typb << 10) | SLP_EN));
    }
    /* Fallbacks for emulators whose _S5 we couldn't parse. */
    outw(0x604,  0x2000);   /* QEMU (newer)            */
    outw(0xB004, 0x2000);   /* QEMU/Bochs (older)      */
    outw(0x4004, 0x3400);   /* VirtualBox              */
    /* If we are still here, ACPI poweroff is unavailable on this platform. */
}

uint32_t acpi_irq_to_gsi(uint8_t irq, uint16_t* flags_out){
    for(uint32_t i=0;i<g_topo.override_count;i++)
        if(g_topo.overrides[i].src_irq == irq){
            if(flags_out) *flags_out = g_topo.overrides[i].flags;
            return g_topo.overrides[i].gsi;
        }
    if(flags_out) *flags_out = 0;
    return irq;                                            /* identity mapping */
}
