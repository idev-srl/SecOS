# M47 — Network stack locking (SMP prerequisite)

**Goal:** make the network stack correct under concurrency so that SMP
(`-DSECOS_SMP_AT_BOOT`, M29) can later be enabled by default. Until now the
stack assumed one CPU: every protocol table and every TX scratch buffer was a
plain static, and correctness relied on luck between the RX path (MSI-X ISR,
timer-tick poll backstop) and process context (syscalls).

## Shared state inventory

| State | Where | Touched from |
|---|---|---|
| NIC RX ring, `dev->poll()` | drivers (e1000/e1000e/vmxnet3/igc) | MSI-X ISR, timer tick, any CPU |
| NIC TX ring, `dev->transmit()` | drivers | every sender (RX path ACKs, syscalls, tick retransmits) |
| socket table + per-socket state, UDP datagram ring | `net/socket.c` | syscalls on every CPU (fds are shared across fork), UDP RX callback |
| TCP control blocks, retransmit / TIME-WAIT / orphan timers | `net/tcp.c` | RX path, tick, socket layer, shell helpers |
| UDP binding table | `net/udp.c` | socket layer, DHCP, DNS, RX demux |
| ARP cache | `net/arp.c` | RX path, every IPv4 send, tick aging, ping |
| DHCP state machine, lease | `net/dhcp.c` | SYS_NETCTL, kernel shell, UDP callback |
| DNS query state (caller's stack) | `net/dns.c` | kernel shell, UDP callback |
| interface L3 config (`ip/netmask/gateway/dns`) | `net_dev_t` | RX path (per frame), every send, netctl, DHCP |
| TX scratch: `g_txframe`, `g_ipbuf`, `g_seg`/`g_cs`, `g_csbuf`, `g_icmpbuf`, `g_pkt`, `g_query` | eth/ipv4/tcp/udp/icmp/dhcp/dns | every sender |
| IPv4 id counter, ICMP echo counter | ipv4/icmp | senders / RX |

Blocked readers are not woken by the RX path: a blocking socket call that
gets `-EAGAIN` sleeps `SOCK_WAIT_TICKS` (2 ms) and re-runs
(`kernel/syscall_trap.c`, unchanged pattern from M24). Because the wait is a
bounded timed sleep and the readiness check is re-done under the stack lock on
every re-run, there is no check-then-block window in which a wakeup could be
lost: at worst a frame that lands right after the check is seen 2 ms later.
The network code therefore never calls into the scheduler while holding a net
lock (and never needs `proc_lock`).

## Lock hierarchy

Always acquired in increasing level, never the reverse (`net/net_lock.h`):

| Level | Lock | Protects |
|---|---|---|
| 0 | tick gate (`g_tick_lock`, trylock) | `net_tick()`: one CPU runs the RX backstop + protocol timers |
| 1 | `dev->rx_lock` (per NIC, trylock) | the driver's RX ring; `dev->poll()` runs only under it |
| 2 | **stack lock** (`net_lock()`) | sockets, TCP control blocks + timers, UDP bindings, ARP cache, DHCP/ICMP state, interface config, all protocol TX scratch |
| 3 | `dev->tx_lock` (per NIC) | the driver's TX ring; `dev->transmit()` runs only under it (`net_dev_xmit()`) |
| leaf | heap lock (`kmalloc`/`kfree` of TCP buffers) | — |

All net locks are IRQ-safe: taken with interrupts off (`pushfq; cli`), because
levels 1–3 are reached from the MSI-X ISR and from the timer tick. The RX path
is `isr_net`/`net_tick` → `net_dev_poll` (L1) → driver poll → `net_rx` (takes
L2 per frame) → eth/arp/ipv4/tcp/udp → reply → `eth_send` → `net_dev_xmit`
(L3). A syscall path is `socket_*` (L2) → tcp/udp → `net_dev_xmit` (L3).
Neither ever takes a lower level while holding a higher one.

Nothing that can sleep, fault on a user address or take a scheduler lock runs
under a net lock: the socket layer works on the syscall layer's kernel bounce
buffers (copies to/from user happen outside), and `copy_to_user` in netctl
runs after the lock is dropped.

**Why one stack lock instead of per-socket locks.** Every send path writes the
same static TX scratch buffers (IP header, TCP segment + pseudo-header, the
Ethernet frame), the RX path touches the socket, TCP and UDP tables in one
pass (UDP callback → socket ring; TCP → control block ← socket), and M25
showed the stack is processing-bound, not lock-bound. A single lock makes the
inversion `udp binding → socket` vs `socket → udp_bind` impossible by
construction. Splitting it (per-socket locks + per-CPU scratch) is a follow-up
if profiling under SMP ever shows contention.

### RX drain without lost kicks

`net_dev_poll(dev)` sets `dev->rx_kick`, then trylocks `rx_lock`. If another
CPU holds it, the caller returns (counted as `rx_rekicks`); the holder loops
`while (rx_kick) { rx_kick = 0; poll(); }`, and re-checks `rx_kick` again
after dropping the lock. A frame that lands while one CPU is draining is
therefore always drained by somebody, without the ISR ever spinning on the
ring. The MSI-X ISR acks the NIC cause unconditionally before the drain.

### Socket lifetime (use-after-close)

A socket slot is reused only after its last fd reference is dropped
(`socket_unref`, refcounted across dup/fork). That release unbinds the UDP
port and hands the TCP control block to `tcp_release()` under the same stack
lock, and the RX path only reaches a socket through a UDP binding or a TCP
control block — both looked up under the lock — so it can never see a freed or
recycled socket. A TCP control block owned by a socket is freed only by
`tcp_release()` or, once orphaned, by `tcp_tick()`; never by the RX path.

### connect() is non-blocking now

The old `socket_connect()` spun in ring 0 (`sti; hlt`) for the whole TCP
handshake — impossible with the stack lock held, and it pinned the CPU for up
to 8 s. It now starts the handshake and returns `-EINPROGRESS`, then
`-EALREADY` while `SYN_SENT`, `0` once established, `-ECONNREFUSED` on RST or
`-ETIMEDOUT` after 20 s. `syscall_trap.c` treats `-EINPROGRESS`/`-EALREADY`
on `SYS_CONNECT` like `-EAGAIN` (sleep + re-run) for a blocking fd; a
`SOCK_NONBLOCK` socket gets the Linux semantics (poll for `POLLOUT`). The ARP
warm-up spin disappeared too: a SYN dropped on an ARP miss is retried every
20 ms by `tcp_tick()`. The kernel shell's blocking helpers (`tcp_connect`,
`tcp_send_all`, `tcp_recv_block`, `tcp_close`, `dns_resolve`, `net_ping_*`,
`dhcp_configure`) take the lock per step and never across a `hlt` wait.

### Lock debugging: `-DSECOS_LOCKDEP_LITE`

Each CPU records a bitmask + stack of the net lock levels it holds. The build
panics (`[LOCKDEP] net: ...`) on an acquisition at or below a held level
(inversion or recursion), on an out-of-order release, and on releasing a net
lock with `IF` set (somebody re-enabled interrupts — e.g. a `sti; hlt` wait —
inside a critical section). `NET_ASSERT_STACK_LOCKED()` in the internals
(`eth_send`, `ipv4_send`, `arp_lookup`, `udp_bind`/`udp_output`, `tcp_xmit`,
`tcp_input`, `tcp_tick`, `tcp_alloc`, the socket lookup, `net_dev_xmit`)
catches an entry point that forgot the lock. Without the define the checks
compile away and a net lock is a plain irqsave spinlock.

## Bugs found and fixed

1. **Timer tick ran N× on N CPUs** (`drivers/timer.c`). Every core's LAPIC
   timer enters `timer_handler`, so `timer_ticks` advanced 4× too fast with
   `-smp 4` and every tick callback (network RX backstop + protocol timers,
   cursor blink) ran on all cores concurrently. Only the BSP now advances the
   tick and runs callbacks; each core still schedules itself.
2. **SMP boot panicked: `[M2][PANIC] vmm_map failed (-3)`**
   (`kernel/process.c`). The reaper detaches a zombie from the process table
   before `process_destroy()` unmaps its kernel stack; another CPU could take
   the same table index (= kernel-stack slot) in that window and map its stack
   over the still-mapped old one (and the late unmap would then have freed the
   new process's stack). Hit every time init started dropbear under
   `-DSECOS_SMP_AT_BOOT -smp 4`. The slot now stays reserved until the stack is
   gone.
3. **Shared TX scratch buffers** (`g_seg`, `g_ipbuf`, `g_txframe`, ...) and all
   protocol tables raced between CPUs and between the ISR and syscalls — the
   locking above.
4. **TCP table exhaustion** — 16 control blocks with static 128 KB buffers ran
   out under a few parallel ssh sessions + a server + TIME-WAIT/orphaned closes
   (`connect` → `-ENOBUFS`). Buffers are now heap-allocated per connection
   (none for a listener) and the table holds 64; sockets and UDP bindings grow
   to 64.
5. **Orphans pinned by a sending peer.** Closing a socket with unread data (or
   receiving new data after close) kept the orphan buffering for the full
   10 s orphan timeout while the peer kept sending. Now RST + immediate free
   (RFC 1122 4.2.2.13, Linux behaviour). When the table is full, the orphaned
   TIME-WAIT closest to expiry is recycled.
6. **Pipe table too small** (`kernel/pipe.c`, 16 → 64): each Dropbear exec
   session holds three pipes, so the fourth parallel ssh session failed with
   `exec request failed on channel 0` — on one CPU as well.
7. `dns_resolve` used a static query buffer (now per call); `netctl SET` read
   the DHCP phase and wrote the config non-atomically (now one critical
   section).

## Stress gate

`/bin/netstress` (signed, CAP_NET) + `tools/net_stress_drive.py`, selftest
"NETSMP gate". The image is built with `-DSECOS_SMP_AT_BOOT
-DSECOS_LOCKDEP_LITE` and a baked throwaway SSH key; everything is driven over
ssh (Dropbear). Per stress round, in parallel:

- `netstress client`: 4 forked workers (pinned round-robin to the CPUs), each
  N rounds of a 16 KB TCP echo round trip (byte pattern verified), a firehose
  connection closed after 8 KB while data is still arriving, a 100 × 512 B UDP
  flood and 3 DNS A queries (answers verified) against host peers on
  10.0.2.2;
- `netstress server` accepting 12 parallel host connections (forked echo
  handlers), every third reset by the host mid-transfer (`SO_LINGER 0`);
- two more ssh sessions doing file I/O.

Before/after every round `netstress stats` (`NETCTL_STATS`) must show the same
socket / TCP / orphan / UDP counts as the baseline, MemFree must stay within
2 MB of the first round (heap high-water slack; a per-connection leak would be
~12 MB/round), and the debugcon log must have no `[LOCKDEP]`, panic or `[EXC]`.
It runs three configurations at once: `-smp 4` e1000e (MSI-X NAPI RX),
`-smp 4` e1000 (polled RX), `-smp 1` e1000e.

### Results

Soak runs (8 client rounds × 4 workers × 3 stress rounds, lockdep on), all
three configurations in parallel: **35/35 driver checks each**, no
`[LOCKDEP]`/panic/`[EXC]`, socket/TCP/orphan/UDP counts back to the baseline
(2/2/0/0: dropbear's listener + the stats session) after every round, MemFree
identical across rounds, `tcp_alloc()` failures 0, peak TCP table use 36–38 of
64. Host side per run: 98 echo + 98 firehose connections, 9 800 UDP datagrams
(all received), 294 DNS answers. Guest-server connections the host resets
before `accept()` are dropped by the kernel (never queued), so `conns=` is
10–12 per round; every non-reset echo connection came back intact.

Before the fixes the same gate showed: SMP boot panic (bug 2), `-ENOBUFS` on
connect (bug 4, then 5 with `tcppeak=64 tcpfull>0`), `exec request failed` on
the fourth ssh session (bug 6, also on `-smp 1`).

Full `tools/selftest.sh` on this branch: **969 passed, 3 failed**, NETSMP gate
80/80. The three failures:

- `PERSIST boot 1 installs the system files (30 programs ...)` — the count
  became 31 with `/bin/netstress`; fixed in the gate afterwards, persist gate
  re-run: 30/30.
- `M29-3 no PMM leak under multicore scheduling` — pre-existing flake (the M8
  demo samples PMM while a zombie pinned to the AP may not be reaped yet): on
  the same loaded host, `main` failed it 3/4 runs, this branch passed 4/4.
- `INIT /bin/init started by the kernel as PID 1` — a `[SCHED] preempt` trace
  landed in the middle of the `[INIT]` debugcon line; the line is intact in
  4/4 standalone boots of this branch (and of `main`).

## Open issues

- RX and the protocol timers run on the BSP only (MSI-X is targeted at the BSP
  and the tick callbacks are BSP-only); the locking allows any CPU to drain,
  but nothing distributes RX yet (RSS / per-queue MSI-X vectors).
- Blocked socket readers still poll every 2 ms instead of being woken by the
  RX path; an event-driven wake needs a wait queue the RX path can signal
  without taking `proc_lock` under the stack lock (defer the wake until after
  `net_unlock`).
- One stack lock: a busy sender holds it (IRQs off) while `tcp_output()` pushes
  up to a 64 KB window through the driver; fine at QEMU/1 GbE rates, a
  candidate for splitting if SMP profiling shows contention.
- The TCP send path still memmoves the whole TX buffer on every ACK.
- `udp_ephemeral_port()` is called unlocked by the kernel shell `udpsend`
  (benign: worst case two callers pick the same candidate and the bind fails).
- SMP is still off by default: this milestone removes the network blockers
  (and two generic SMP bugs found on the way), not the M29 real-hardware
  freeze (TLB shootdown, IPI reschedule).
