/* <sys/sysinfo.h> — SecOS libc (SYS_SYSINFO 79, Linux layout). SPDX: MIT */
#ifndef _SYS_SYSINFO_H
#define _SYS_SYSINFO_H
#define SI_LOAD_SHIFT 16
struct sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
    char _f[4];
};
int sysinfo(struct sysinfo* info);
int get_nprocs(void);
int get_nprocs_conf(void);
#endif
