/*
 * SecOS Kernel - owner/mode mount options for filesystems without owners
 * Copyright (c) 2026 iDev srl
 * SPDX-License-Identifier: MIT
 *
 * FAT32, exFAT and NTFS store no Unix owner or mode, so (as Linux vfat, exfat
 * and ntfs3 do) the mount supplies them: every directory is uid:gid with mode
 * 0777 & ~dmask, every file 0777 & ~fmask. mount(2) data, comma-separated:
 *
 *   uid=N  gid=N  umask=OOO (both masks)  dmask=OOO  fmask=OOO
 *   ro  rw  defaults
 *
 * Defaults: root:root, dmask 022, fmask 0133 -> directories 0755, files 0644
 * (no exec bits: programs are not run from these volumes). So any user can
 * read a mounted USB stick / data disk and only root writes it, unless the
 * mount says otherwise (e.g. `mount -o uid=1000,gid=1000 /dev/usb0 /media`).
 * A file the FS marks read-only (FAT/exFAT ATTR_READ_ONLY, NTFS
 * FILE_ATTRIBUTE_READONLY) loses its write bits, as on Linux.
 *
 * Header-only (static inline) so the host fuzz harnesses that compile one
 * driver .c on its own need nothing extra.
 */
#ifndef SECOS_FS_MNTOPTS_H
#define SECOS_FS_MNTOPTS_H

#include <stdint.h>

typedef struct {
    uint32_t uid, gid, dmask, fmask;
    int ro;        /* "ro" given */
    int rw;        /* "rw" given explicitly */
} mntopts_t;

static inline void mntopts_default(mntopts_t* o){
    o->uid = 0; o->gid = 0; o->dmask = 022; o->fmask = 0133; o->ro = 0; o->rw = 0;
}

static inline int mntopts_num_(const char** pp, uint32_t base, uint32_t* out){
    const char* p = *pp; uint32_t v = 0; int n = 0;
    while(*p >= '0' && *p <= (base == 8 ? '7' : '9')){
        uint32_t d = (uint32_t)(*p - '0');
        if(v > (0xFFFFFFFFu - d) / base) return -1;          /* overflow */
        v = v * base + d; p++; n++;
    }
    if(!n) return -1;
    *out = v; *pp = p; return 0;
}

static inline int mntopts_key_(const char** pp, const char* k){
    const char* p = *pp;
    while(*k){ if(*p != *k) return 0; p++; k++; }
    *pp = p; return 1;
}

/* Parse `s` (NULL/"" = defaults) into `o`. 0, or -1 for an unknown or
 * malformed option (the mount is then refused with EINVAL). */
static inline int mntopts_parse(const char* s, mntopts_t* o){
    mntopts_t t; mntopts_default(&t);
    const char* p = s ? s : "";
    while(*p){
        uint32_t v;
        if(mntopts_key_(&p, "uid="))        { if(mntopts_num_(&p, 10, &v)) return -1; t.uid = v; }
        else if(mntopts_key_(&p, "gid="))   { if(mntopts_num_(&p, 10, &v)) return -1; t.gid = v; }
        else if(mntopts_key_(&p, "umask=")) { if(mntopts_num_(&p, 8, &v)) return -1; t.dmask = t.fmask = v & 0777u; }
        else if(mntopts_key_(&p, "dmask=")) { if(mntopts_num_(&p, 8, &v)) return -1; t.dmask = v & 0777u; }
        else if(mntopts_key_(&p, "fmask=")) { if(mntopts_num_(&p, 8, &v)) return -1; t.fmask = v & 0777u; }
        else if(mntopts_key_(&p, "defaults")){ }
        else if(mntopts_key_(&p, "ro"))     { t.ro = 1; t.rw = 0; }
        else if(mntopts_key_(&p, "rw"))     { t.rw = 1; t.ro = 0; }
        else return -1;
        if(*p == ',') p++; else if(*p) return -1;
    }
    *o = t;
    return 0;
}

/* Full st_mode for a directory / file of such a volume. */
static inline uint32_t mntopts_mode(const mntopts_t* o, int is_dir, int fs_readonly_attr){
    if(is_dir) return 0040000u | (0777u & ~o->dmask);
    uint32_t m = 0777u & ~o->fmask;
    if(fs_readonly_attr) m &= ~0222u;
    return 0100000u | m;
}

#endif
