/* SecOS [M46] exec confinement selftest (user side).
 *
 * One source, linked three times with different signed manifests:
 *   m46conf  - CONFINED (CAP_ENFORCE|FS_READ|PROC|IPC|SIGNAL|TIME): the driver.
 *   m46probe - ambient + explicit CAP_NET|CAP_INPUT|CAP_DISPLAY: what it may do
 *              depends only on who started it.
 *   m46setid - MANIFEST_FLAG_SETID, installed root 04755: reports its euid.
 * Every result line starts with "[m46] " so the selftest can grep debugcon.
 *
 * Modes (argv[1]):
 *   (none)    confined driver: caps, then spawn/fork/execve of ambient programs
 *             (m46probe, busybox, bash) and of the set-id program as uid 1000.
 *   probe TAG try socket, /dev/fb0, create+write /tmp/m46_TAG; print the results.
 *   ctl       ambient control: probe TAG=ctl via spawn, set-id as uid 1000 works,
 *             then voluntary no_new_privs (prctl) closes both.
 *   setid TAG print uid/euid.
 */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include "libsecos.h"

#define SYS_PRCTL           90
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#define PR_SECOS_GET_CAPS   0x53430001L

static long caps(void) { return secos_syscall(SYS_PRCTL, PR_SECOS_GET_CAPS, 0, 0, 0, 0); }
static long nnp(void)  { return secos_syscall(SYS_PRCTL, PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0); }

static void report(const char* tag) {
    dprintf(1, "[m46] %s caps=0x%lx nnp=%ld uid=%d euid=%d\n", tag, caps(), nnp(), getuid(), geteuid());
}

static int run(const char* path, char* const argv[]) {
    int pid = spawn(path, argv);
    if (pid < 0) return -1000 + pid;
    int st = waitpid(pid);
    return (st >> 8) & 0xff;
}

static int exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int probe(const char* tag) {
    char path[64];
    report(tag);
    int s = socket(SOCK_DGRAM);
    dprintf(1, "[m46] %s socket=%s\n", tag, s >= 0 ? "OK" : "DENIED");
    if (s >= 0) sockclose(s);
    /* fb0 exists only with a framebuffer; the PS/2 keyboard's evdev node always
     * does. Both are explicit caps (DISPLAY / INPUT): denial is EACCES. */
    int fb = open("/dev/fb0", O_RDONLY);
    dprintf(1, "[m46] %s fb0=%s\n", tag, fb >= 0 ? "OK" : errno == 13 ? "DENIED" : "ABSENT");
    if (fb >= 0) close(fb);
    int ev = open("/dev/input/event0", O_RDONLY);
    dprintf(1, "[m46] %s input=%s\n", tag, ev >= 0 ? "OK" : errno == 13 ? "DENIED" : "ABSENT");
    if (ev >= 0) close(ev);
    snprintf(path, sizeof(path), "/tmp/m46_%s", tag);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int w = fd >= 0 ? (int)write(fd, "x\n", 2) : -1;
    if (fd >= 0) close(fd);
    dprintf(1, "[m46] %s write=%s\n", tag, w == 2 ? "OK" : "DENIED");
    return 0;
}

static void setid_as_1000(const char* tag) {
    char* av[] = { "m46setid", "setid", (char*)tag, 0 };
    int pid = fork();
    if (pid == 0) {
        if (setuid(1000) != 0) { dprintf(1, "[m46] %s setuid failed\n", tag); _exit(1); }
        int sp = spawn("/bin/m46setid", av);           /* SYS_SPAWN path */
        if (sp >= 0) waitpid(sp);
        char tag2[32];
        snprintf(tag2, sizeof(tag2), "%s-exec", tag);
        char* av2[] = { "m46setid", "setid", tag2, 0 };
        execve("/bin/m46setid", av2, 0);               /* in-place execve path */
        dprintf(1, "[m46] %s execve failed\n", tag);
        _exit(1);
    }
    if (pid > 0) waitpid(pid);
}

static int driver(void) {
    report("conf");
    /* 1. spawn an ambient program with explicit caps: it must get none of them. */
    char* pa[] = { "m46probe", "probe", "spawn", 0 };
    run("/bin/m46probe", pa);
    /* 2. fork: the child is a copy (same confinement). */
    int pid = fork();
    if (pid == 0) { report("fork"); _exit(0); }
    if (pid > 0) waitpid(pid);
    /* 3. fork + execve the ambient probe. */
    pid = fork();
    if (pid == 0) {
        char* ea[] = { "m46probe", "probe", "exec", 0 };
        execve("/bin/m46probe", ea, 0);
        dprintf(1, "[m46] execve failed\n");
        _exit(1);
    }
    if (pid > 0) waitpid(pid);
    /* 4. ambient BusyBox and bash must not write files either. */
    char* bb[] = { "busybox", "touch", "/tmp/m46_busybox", 0 };
    int rc = run("/bin/busybox", bb);
    dprintf(1, "[m46] busybox touch rc=%d file=%s\n", rc, exists("/tmp/m46_busybox") ? "CREATED" : "ABSENT");
    char* sh[] = { "bash", "-c", "echo hi > /tmp/m46_bash", 0 };
    rc = run("/bin/bash", sh);
    dprintf(1, "[m46] bash redirect rc=%d file=%s\n", rc, exists("/tmp/m46_bash") ? "CREATED" : "ABSENT");
    /* 5. the set-id program, run as uid 1000, must not give euid 0. */
    setid_as_1000("conf-setid");
    /* 6. no_new_privs cannot be cleared. */
    long r = secos_syscall(SYS_PRCTL, PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0);
    dprintf(1, "[m46] clear nnp rc=%ld nnp=%ld\n", r, nnp());
    dprintf(1, "[m46] DONE conf\n");
    return 0;
}

static int control(void) {
    report("ctl");
    char* pa[] = { "m46probe", "probe", "ctl", 0 };
    run("/bin/m46probe", pa);
    setid_as_1000("ctl-setid");
    long r = secos_syscall(SYS_PRCTL, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    dprintf(1, "[m46] set nnp rc=%ld nnp=%ld\n", r, nnp());
    char* pb[] = { "m46probe", "probe", "nnp", 0 };
    run("/bin/m46probe", pb);
    setid_as_1000("nnp-setid");
    dprintf(1, "[m46] DONE ctl\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "probe") == 0) return probe(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "setid") == 0) { report(argv[2]); return 0; }
    if (argc >= 2 && strcmp(argv[1], "ctl") == 0) return control();
    return driver();
}
