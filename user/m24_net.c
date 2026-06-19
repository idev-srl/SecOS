/* SecOS [M24] networking + CAP_NET demo (ring 3, signed).
 *
 * Proves the capability gate: socket() succeeds only if the signed manifest
 * grants CAP_NET. Built twice — once with the CAP_NET manifest (note_net.S),
 * once without (note.S) — so the kernel demo can show OK vs DENIED. With a NIC
 * configured (dhcp from the shell) it also fires a best-effort UDP datagram to
 * prove the data path reaches the wire.
 * SPDX-License-Identifier: MIT */
#include "libsecos.h"

int main(void) {
    int s = socket(SOCK_DGRAM);
    if (s < 0) {
        puts("[m24net] socket DENIED (no CAP_NET)");
        return 1;
    }
    puts("[m24net] socket OK (CAP_NET granted)");

    /* Best-effort: send one datagram to 10.0.2.2:9 (discard). Harmless without a
     * NIC/route (returns < 0); proves the send path when networking is up. */
    struct secos_sockaddr sa;
    sa.ip = ip4(10, 0, 2, 2);
    sa.port = 9;
    sa._pad = 0;
    long n = sendto(s, "hi", 2, &sa);
    if (n == 2) puts("[m24net] udp sendto OK");

    sockclose(s);
    return 0;
}
