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

/* Read ACPI tables through the physmap only. [M44] The low identity map
 * (0-512 MiB) used to serve tables below 512 MiB; it is retired after early
 * boot, and some ACPI users run later (the FADT reset register at reboot, the
 * shell `smp` bring-up), so every table access goes through the physmap.
 *
 * [UEFI-FIX] On large-RAM UEFI machines (e.g. 2 GB+, including the ASUS test
 * laptop) the firmware places the ACPI tables in RESERVED memory ABOVE the usable
 * RAM top, which the physmap does not cover. [M49] So every table is mapped
 * explicitly, write-back and only where it is not mapped yet (vmm_map_phys):
 * pv() maps the first bytes (an RSDP or a table header), pvt() maps a whole SDT
 * once its header gives the length. No speculative margin: the physmap never
 * covers MMIO with a WB mapping. */
static const uint8_t* pv(uint64_t phys){
    vmm_map_phys(phys, 64);
    return (const uint8_t*)phys_to_virt(phys);
}
static uint32_t rd32(const uint8_t* p);
static const uint8_t* pvt(uint64_t phys){
    const uint8_t* h = pv(phys);
    uint32_t len = rd32(h + 4);
    if(len > 36 && len <= (16u << 20)) vmm_map_phys(phys, len);
    return h;
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
    const uint8_t* t = pvt(madt);
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
    const uint8_t* h = pvt(sdt);
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
    const uint8_t* f = pvt(fadt);
    uint32_t flen = rd32(f + 4);
    g_topo.pm1a_cnt = rd32(f + 64);     /* PM1a_CNT_BLK */
    g_topo.pm1b_cnt = rd32(f + 68);     /* PM1b_CNT_BLK */
    /* ACPI 2.0+: X_PM1a/b_CNT_BLK (12-byte GAS @172/@184). Used only when the
     * 32-bit field is empty and the register is in system I/O space (id 1). */
    if(flen >= 196){
        if(!g_topo.pm1a_cnt && f[172] == 1) g_topo.pm1a_cnt = (uint32_t)rd64(f + 176);
        if(!g_topo.pm1b_cnt && f[184] == 1) g_topo.pm1b_cnt = (uint32_t)rd64(f + 188);
    }
    g_topo.smi_cmd     = rd32(f + 48);  /* SMI_CMD */
    g_topo.acpi_enable = f[52];         /* ACPI_ENABLE */
    /* Reset register (ACPI 2.0+, FADT >= 129 bytes). RESET_REG is a 12-byte GAS
     * at offset 116 (AddressSpaceId @116, Address @120), RESET_VALUE @128; FADT
     * flags @112 bit10 (RESET_REG_SUP) gates support. */
    if(rd32(f+4) >= 129 && (rd32(f+112) & (1u<<10))){
        uint64_t raddr = rd64(f + 120);
        if(raddr){
            g_topo.reset_supported = 1;
            g_topo.reset_space     = f[116];
            g_topo.reset_addr      = raddr;
            g_topo.reset_value     = f[128];
        }
    }
    uint64_t dsdt = rd32(f + 40);       /* DSDT (32-bit) */
    if(rd32(f+4) >= 148){               /* FADT long enough for X_DSDT (64-bit) */
        uint64_t xdsdt = rd64(f + 140);
        if(xdsdt) dsdt = xdsdt;
    }
    if(!dsdt) return;
    const uint8_t* d = pvt(dsdt);
    uint32_t dlen = rd32(d + 4);
    /* Find the object "Name (_S5, Package () {SLP_TYPa, SLP_TYPb, ...})":
     * NameOp (0x08), optionally a root prefix '\\', the NameSeg "_S5_", then
     * PackageOp (0x12) PkgLength NumElements and the elements. A "_S5_" that is
     * not a NameOp target (a reference inside a method, a string) is skipped. */
    for(uint32_t i = 38; i + 16 < dlen; i++){
        if(!(d[i]=='_' && d[i+1]=='S' && d[i+2]=='5' && d[i+3]=='_')) continue;
        if(!(d[i-1] == 0x08 || (d[i-1] == '\\' && d[i-2] == 0x08))) continue;
        const uint8_t* p = d + i + 4;
        if(*p != 0x12) continue;          /* PackageOp expected */
        p++;
        p += 1 + (*p >> 6);               /* PkgLength: lead byte + 0..3 more bytes */
        p++;                              /* NumElements */
        /* Elements 0/1: Zero (0x00), One (0x01), BytePrefix (0x0A), WordPrefix
         * (0x0B) or DWordPrefix (0x0C) constants. Only bits 0-2 are SLP_TYP. */
        int ok = 1;
        for(int e=0; e<2; e++){
            uint32_t v = 0;
            if(*p == 0x00 || *p == 0x01){ v = *p++; }
            else if(*p == 0x0A){ v = p[1]; p += 2; }
            else if(*p == 0x0B){ v = rd16(p + 1); p += 3; }
            else if(*p == 0x0C){ v = rd32(p + 1); p += 5; }
            else { ok = (e == 1); break; }   /* SLP_TYPb may be absent / non-constant */
            if(e==0) g_topo.slp_typa = (uint16_t)(v & 7); else g_topo.slp_typb = (uint16_t)(v & 7);
        }
        if(!ok) continue;
        g_topo.s5_ok = (g_topo.pm1a_cnt != 0);
        return;
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
static inline uint16_t inw(uint16_t port){ uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline void outb_p(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
/* ~1 ms per step: the PIT-independent port-0x80 delay (about 1 us per access). */
static void acpi_delay_ms(int ms){
    for(int i = 0; i < ms * 1000; i++) __asm__ volatile("outb %%al,$0x80"::"a"(0));
}

/* S5 (soft-off). The write that powers off is asynchronous on some platforms
 * (QEMU under TCG processes the shutdown request on its main loop while the
 * vCPU keeps running), so each attempt is followed by a wait before the next
 * one, and failure is declared only after the last. */
void acpi_poweroff(void){
    const uint16_t SLP_EN = 1u << 13, SCI_EN = 1u << 0;
    debugcon_writestring("[ACPI] poweroff: pm1a="); debugcon_print_hex(g_topo.pm1a_cnt);
    debugcon_writestring(" slp_typa="); debugcon_print_hex(g_topo.slp_typa);
    debugcon_writestring(" s5_ok="); debugcon_print_hex((uint64_t)g_topo.s5_ok);
    debugcon_writestring("\n");
    if(g_topo.s5_ok && g_topo.pm1a_cnt){
        uint16_t a = (uint16_t)g_topo.pm1a_cnt, b = (uint16_t)g_topo.pm1b_cnt;
        /* Legacy firmware boots in SMM/legacy mode (SCI_EN clear): switch to
         * ACPI mode first (FADT SMI_CMD <- ACPI_ENABLE), or a real chipset
         * ignores SLP_EN. Bounded wait for SCI_EN. */
        if(!(inw(a) & SCI_EN) && g_topo.smi_cmd && g_topo.acpi_enable){
            outb_p((uint16_t)g_topo.smi_cmd, g_topo.acpi_enable);
            for(int i = 0; i < 300 && !(inw(a) & SCI_EN); i++) acpi_delay_ms(1);
        }
        /* SLP_TYP is bits 10-12; keep the other bits (SCI_EN etc.). */
        uint16_t va = (uint16_t)((inw(a) & ~(7u << 10)) | ((g_topo.slp_typa & 7u) << 10));
        uint16_t vb = b ? (uint16_t)((inw(b) & ~(7u << 10)) | ((g_topo.slp_typb & 7u) << 10)) : 0;
        outw(a, va);                               /* SLP_TYP first, then SLP_EN */
        if(b) outw(b, vb);
        outw(a, (uint16_t)(va | SLP_EN));
        if(b) outw(b, (uint16_t)(vb | SLP_EN));
        acpi_delay_ms(500);
        debugcon_writestring("[ACPI] S5 via PM1 did not power off; trying emulator ports\n");
    }
    /* Fallbacks for emulators whose _S5 we couldn't parse. */
    outw(0x604,  0x2000);   /* QEMU (newer)            */
    outw(0xB004, 0x2000);   /* QEMU/Bochs (older)      */
    outw(0x4004, 0x3400);   /* VirtualBox              */
    acpi_delay_ms(500);
    /* If we are still here, ACPI poweroff is unavailable on this platform. */
}

static inline void outb_(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static inline uint8_t inb_(uint16_t port){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline void reboot_delay(void){ for(volatile int i=0;i<2000000;i++){ } }

void acpi_reboot(void){
    debugcon_writestring("[ACPI] reboot: reset_sup="); debugcon_print_hex(g_topo.reset_supported);
    debugcon_writestring(" space="); debugcon_print_hex(g_topo.reset_space);
    debugcon_writestring(" addr="); debugcon_print_hex(g_topo.reset_addr);
    debugcon_writestring(" val="); debugcon_print_hex(g_topo.reset_value);
    debugcon_writestring("\n");
    __asm__ volatile("cli");

    /* 1. FADT reset register — the method most modern UEFI laptops actually honor. */
    if(g_topo.reset_supported){
        if(g_topo.reset_space == 1){                 /* SystemIO */
            outb_((uint16_t)g_topo.reset_addr, g_topo.reset_value);
        } else if(g_topo.reset_space == 0){          /* SystemMemory */
            volatile uint8_t* r = (volatile uint8_t*)vmm_map_mmio(g_topo.reset_addr, 1, VMM_CACHE_UC);
            if(r) *r = g_topo.reset_value;
        }
        reboot_delay();
    }

    /* 2. PCI reset control register (0xCF9): full-reset (0x06) and hard-reset (0x0E). */
    outb_(0xCF9, 0x02); reboot_delay();
    outb_(0xCF9, 0x06); reboot_delay();
    outb_(0xCF9, 0x0E); reboot_delay();

    /* 3. Legacy 8042 keyboard-controller pulse (does nothing on the ASUS, kept for
     *    older machines / emulators). Drain the input buffer, then pulse reset. */
    for(int i=0;i<100000;i++){ if(!(inb_(0x64) & 0x02)) break; }
    outb_(0x64, 0xFE); reboot_delay();

    /* 4. Triple fault: load a zero-length IDT and raise an interrupt. With no valid
     *    handler the CPU faults thrice and the firmware resets the machine — the
     *    guaranteed last resort. */
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int3" :: "m"(null_idt));
    for(;;) __asm__ volatile("hlt");
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
