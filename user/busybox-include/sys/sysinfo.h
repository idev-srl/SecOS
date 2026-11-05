/* SecOS BusyBox overlay: <sys/sysinfo.h> (sysinfo() is assembled from
 * /proc/uptime + /proc/meminfo by the shim). SPDX-License-Identifier: MIT */
#ifndef _BB_SYS_SYSINFO_H
#define _BB_SYS_SYSINFO_H
struct sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
    char _f[20 - 2 * sizeof(long) - sizeof(int)];
};
#define SI_LOAD_SHIFT 16
int sysinfo(struct sysinfo* info);
int get_nprocs(void);
int get_nprocs_conf(void);
long get_phys_pages(void);
long get_avphys_pages(void);
#endif
