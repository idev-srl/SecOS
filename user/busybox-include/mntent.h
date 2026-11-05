/* SecOS BusyBox overlay: <mntent.h> (getmntent over /proc/mounts).
 * SPDX-License-Identifier: MIT */
#ifndef _BB_MNTENT_H
#define _BB_MNTENT_H
#include <stdio.h>
#include <paths.h>
#define MOUNTED _PATH_MOUNTED
#define MNTTAB  _PATH_MNTTAB
#define MNTTYPE_SWAP "swap"
#define MNTOPT_DEFAULTS "defaults"
#define MNTOPT_RO "ro"
#define MNTOPT_RW "rw"
#define MNTOPT_NOAUTO "noauto"
struct mntent { char* mnt_fsname; char* mnt_dir; char* mnt_type; char* mnt_opts; int mnt_freq; int mnt_passno; };
FILE*          setmntent(const char* file, const char* mode);
struct mntent* getmntent(FILE* f);
struct mntent* getmntent_r(FILE* f, struct mntent* m, char* buf, int len);
int            addmntent(FILE* f, const struct mntent* m);
int            endmntent(FILE* f);
char*          hasmntopt(const struct mntent* m, const char* opt);
#endif
