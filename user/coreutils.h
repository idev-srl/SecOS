/* SecOS coreutils — a busybox-style multi-call binary. Each applet is
 * `int applet_NAME(int argc, char** argv)` (argv[0] = applet name), returning an
 * exit code. The dispatcher (coreutils.c) picks the applet from basename(argv[0])
 * or, failing that, argv[1]. SPDX-License-Identifier: MIT */
#ifndef COREUTILS_H
#define COREUTILS_H

/* --- text processing (cu_text.c) --- */
int applet_echo(int, char**);
int applet_cat(int, char**);
int applet_wc(int, char**);
int applet_head(int, char**);
int applet_tail(int, char**);
int applet_rev(int, char**);
int applet_nl(int, char**);
int applet_yes(int, char**);
int applet_seq(int, char**);

/* --- filesystem (cu_file.c) --- */
int applet_ls(int, char**);
int applet_mkdir(int, char**);
int applet_rm(int, char**);
int applet_touch(int, char**);
int applet_cp(int, char**);
int applet_basename(int, char**);
int applet_dirname(int, char**);
int applet_stat(int, char**);

/* --- system / misc (cu_misc.c) --- */
int applet_true(int, char**);
int applet_false(int, char**);
int applet_uname(int, char**);
int applet_sleep(int, char**);
int applet_clear(int, char**);
int applet_hexdump(int, char**);
int applet_uptime(int, char**);
int applet_help(int, char**);

#endif
