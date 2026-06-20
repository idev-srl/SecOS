/* SecOS coreutils dispatcher (busybox-style multi-call binary). SPDX: MIT */
#include <stdio.h>
#include <string.h>
#include "coreutils.h"

static const struct { const char* name; int (*fn)(int, char**); } applets[] = {
    {"echo", applet_echo}, {"cat", applet_cat}, {"wc", applet_wc},
    {"head", applet_head}, {"tail", applet_tail}, {"rev", applet_rev},
    {"nl", applet_nl}, {"yes", applet_yes}, {"seq", applet_seq},
    {"ls", applet_ls}, {"mkdir", applet_mkdir}, {"rm", applet_rm},
    {"touch", applet_touch}, {"cp", applet_cp}, {"basename", applet_basename},
    {"dirname", applet_dirname}, {"stat", applet_stat},
    {"true", applet_true}, {"false", applet_false}, {"uname", applet_uname},
    {"sleep", applet_sleep}, {"clear", applet_clear}, {"hexdump", applet_hexdump},
    {"uptime", applet_uptime}, {"help", applet_help}, {"coreutils", applet_help},
    {0, 0}
};

static const char* base(const char* p) {
    const char* b = p; for (; *p; p++) if (*p == '/') b = p + 1; return b;
}
static int (*lookup(const char* name))(int, char**) {
    for (int i = 0; applets[i].name; i++) if (!strcmp(applets[i].name, name)) return applets[i].fn;
    return 0;
}

int applet_help(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("SecOS coreutils — applets:\n");
    for (int i = 0; applets[i].name; i++) printf(" %s", applets[i].name);
    printf("\nUsage: run /bin/<applet> [args]   (or: run /bin/coreutils <applet> [args])\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 1) return applet_help(0, 0);
    const char* b = base(argv[0]);
    int (*fn)(int, char**) = 0;
    /* When invoked as the bare binary ("coreutils"), the applet is argv[1] so
     * `run /bin/coreutils ls /` works; otherwise dispatch on the link name. */
    if (strcmp(b, "coreutils") != 0) fn = lookup(b);
    if (!fn && argc >= 2) { fn = lookup(argv[1]); if (fn) { argv++; argc--; } }
    if (!fn) { printf("coreutils: unknown applet '%s'\n", b); return 1; }
    return fn(argc, argv);
}
