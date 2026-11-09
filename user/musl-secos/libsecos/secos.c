/* libsecos.a for musl programs: the SecOS-only calls of <secos/secos.h>.
 * SPDX-License-Identifier: MIT */
#include <secos/secos.h>

long secos_syscall(long num, long a0, long a1, long a2, long a3, long a4, long a5)
{
    long ret;
    register long r8 __asm__("r8") = a4;
    register long r9 __asm__("r9") = a5;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "0"(num), "D"(a0), "S"(a1), "d"(a2), "c"(a3), "r"(r8), "r"(r9)
                         : "memory", "cc");
    return ret;
}

int secos_spawn(const char *path, char *const argv[])
{ return (int)secos_syscall(8, (long)path, (long)argv, 0, 0, 0, 0); }
int secos_wait(int pid)
{ return (int)secos_syscall(9, pid, 0, 0, 0, 0, 0); }
unsigned long secos_getticks(void)
{ return (unsigned long)secos_syscall(10, 0, 0, 0, 0, 0, 0); }
void secos_sleep_ticks(unsigned long ticks)
{ secos_syscall(13, (long)ticks, 0, 0, 0, 0, 0); }
long secos_msg_send(int chan, const void *buf, long len)
{ return secos_syscall(11, chan, (long)buf, len, 0, 0, 0); }
long secos_msg_recv(int chan, void *buf, long len)
{ return secos_syscall(12, chan, (long)buf, len, 0, 0, 0); }
long secos_netctl(int op, int ifindex, void *arg)
{ return secos_syscall(95, op, ifindex, (long)arg, 0, 0, 0); }
long secos_get_caps(void)
{ return secos_syscall(90, 0x53430001L, 0, 0, 0, 0, 0); }
int secos_reboot(int cmd)
{ return (int)secos_syscall(59, cmd, 0, 0, 0, 0, 0); }
int secos_sigverify(const void *digest32, const void *sig64, unsigned key_id)
{ return (int)secos_syscall(96, (long)digest32, (long)sig64, (long)key_id, 0, 0, 0); }
