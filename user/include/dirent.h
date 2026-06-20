/* <dirent.h> — SecOS libc (over SYS_GETDENTS). SPDX-License-Identifier: MIT */
#ifndef _DIRENT_H
#define _DIRENT_H

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4
#define DT_LNK     10

struct dirent {
    unsigned char d_type;
    char d_name[256];
};

typedef struct {
    char*  buf;     /* packed 256-byte kernel records */
    int    len;     /* bytes filled */
    int    pos;     /* current offset */
    struct dirent ent;
} DIR;

DIR* opendir(const char* path);
struct dirent* readdir(DIR* d);
int  closedir(DIR* d);

#endif
