/*
 * SecOS Kernel - MBR/GPT partition table parsing. See partition.h.
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "partition.h"
#include "debugcon.h"

/* A partition presented as a block device. block_dev_t MUST be the first member
 * so a (partition_dev_t*) can be recovered from the read/write callback's dev. */
typedef struct {
    block_dev_t  dev;
    block_dev_t* parent;
    uint64_t     start_lba;
    char         name[20];
} partition_dev_t;

#define MAX_PARTS 16
static partition_dev_t g_parts[MAX_PARTS];
static int g_part_count = 0;

static uint32_t rd32le(const uint8_t* p){
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint64_t rd64le(const uint8_t* p){
    uint64_t v = 0; for(int i=7;i>=0;i--) v = (v<<8) | p[i]; return v;
}

static int part_read(block_dev_t* d, uint64_t lba, void* buf, uint32_t count){
    partition_dev_t* p = (partition_dev_t*)d;
    if(count == 0) return 0;
    if(lba + count > d->sector_count) return -1;       /* bound to the partition */
    return p->parent->read(p->parent, p->start_lba + lba, buf, count);
}
static int part_write(block_dev_t* d, uint64_t lba, const void* buf, uint32_t count){
    partition_dev_t* p = (partition_dev_t*)d;
    if(count == 0) return 0;
    if(lba + count > d->sector_count) return -1;
    if(!p->parent->write) return -1;
    return p->parent->write(p->parent, p->start_lba + lba, buf, count);
}

/* Register one partition sub-device "<parent>p<num>" covering [start, start+count). */
static int add_partition(block_dev_t* parent, uint64_t start, uint64_t count, int num){
    if(g_part_count >= MAX_PARTS || count == 0) return 0;
    partition_dev_t* p = &g_parts[g_part_count];
    /* build name: parent name + 'p' + decimal num */
    int n = 0; const char* s = parent->name;
    while(s[n] && n < 15){ p->name[n] = s[n]; n++; }
    /* Linux convention: insert 'p' only when the disk name ends in a digit
     * (nvme0n1p1, usb0p1) — otherwise append directly (sda1, vda1). */
    if(n > 0 && p->name[n-1] >= '0' && p->name[n-1] <= '9') p->name[n++] = 'p';
    char tmp[8]; int t = 0; int v = num;
    if(v == 0) tmp[t++] = '0';
    while(v){ tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while(t > 0 && n < 19) p->name[n++] = tmp[--t];
    p->name[n] = 0;

    p->parent        = parent;
    p->start_lba     = start;
    p->dev.name        = p->name;
    p->dev.sector_size = parent->sector_size;
    p->dev.sector_count= count;
    p->dev.read        = part_read;
    p->dev.write       = parent->write ? part_write : 0;
    if(block_register(&p->dev) != 0) return 0;          /* registry full */
    g_part_count++;
    debugcon_writestring("[PART] "); debugcon_writestring(p->name);
    debugcon_writestring(" start="); debugcon_print_hex(start);
    debugcon_writestring(" count="); debugcon_print_hex(count);
    debugcon_writestring("\n");
    return 1;
}

/* Parse the GPT (header at LBA 1, entries from PartitionEntryLBA). */
static int probe_gpt(block_dev_t* parent, uint32_t ss){
    static uint8_t hdr[4096];
    if(block_read(parent, 1, hdr, 1) != 1) return 0;
    static const char sig[8] = {'E','F','I',' ','P','A','R','T'};
    for(int i=0;i<8;i++) if(hdr[i] != (uint8_t)sig[i]) return 0;
    uint64_t ent_lba = rd64le(hdr + 72);
    uint32_t num_ent = rd32le(hdr + 80);
    uint32_t ent_sz  = rd32le(hdr + 84);
    if(ent_sz < 128 || ent_sz > 512) return 0;
    if(num_ent > 256) num_ent = 256;                    /* cap the scan */
    uint32_t per_sec = ss / ent_sz; if(per_sec == 0) per_sec = 1;
    static uint8_t secbuf[4096];
    int added = 0, num = 0;
    for(uint32_t i = 0; i < num_ent && g_part_count < MAX_PARTS; ){
        if(block_read(parent, ent_lba + i / per_sec, secbuf, 1) != 1) break;
        for(uint32_t j = 0; j < per_sec && i < num_ent; j++, i++){
            const uint8_t* e = secbuf + j * ent_sz;
            num++;                                       /* GPT partition number = slot index (1-based) */
            int used = 0; for(int k=0;k<16;k++) if(e[k]){ used = 1; break; } /* type GUID != 0 */
            if(!used) continue;
            uint64_t first = rd64le(e + 32);
            uint64_t last  = rd64le(e + 40);
            if(last < first) continue;
            added += add_partition(parent, first, last - first + 1, num);
        }
    }
    return added;
}

/* Parents already probed — so a re-scan (usbrescan / hot-plug) re-probes only the
 * newly-appeared disks instead of duplicating partitions of the existing ones. */
static block_dev_t* g_probed[24];
static int g_nprobed = 0;

int block_probe_partitions(block_dev_t* parent){
    if(!parent || !parent->read) return 0;
    for(int i=0;i<g_nprobed;i++) if(g_probed[i]==parent) return 0; /* already done */
    if(g_nprobed < (int)(sizeof(g_probed)/sizeof(g_probed[0]))) g_probed[g_nprobed++]=parent;
    uint32_t ss = parent->sector_size;
    if(ss < 512 || ss > 4096) return 0;
    static uint8_t sec0[4096];
    if(block_read(parent, 0, sec0, 1) != 1) return 0;
    if(!(sec0[510] == 0x55 && sec0[511] == 0xAA)) return 0;   /* no MBR boot signature */

    /* GPT: a protective/hybrid MBR carries a type-0xEE entry; the real table is GPT. */
    if(sec0[446 + 4] == 0xEE){
        int g = probe_gpt(parent, ss);
        if(g) return g;
        /* fall through: some hybrids also list usable MBR entries */
    }
    /* Classic MBR: 4 primary entries of 16 bytes at offset 446. */
    int added = 0;
    for(int i = 0; i < 4; i++){
        const uint8_t* e = sec0 + 446 + i * 16;
        uint8_t type = e[4];
        if(type == 0) continue;                          /* empty */
        if(type == 0x05 || type == 0x0F || type == 0x85) continue; /* extended container (v0: skip) */
        if(type == 0xEE) continue;                       /* already handled as GPT */
        uint32_t start = rd32le(e + 8);
        uint32_t cnt   = rd32le(e + 12);
        added += add_partition(parent, start, cnt, i + 1);
    }
    return added;
}

void block_scan_partitions(void){
    int n = block_count();          /* snapshot: only base disks registered so far */
    for(int i = 0; i < n; i++){
        block_dev_t* d = block_get(i);
        if(d) block_probe_partitions(d);
    }
}
