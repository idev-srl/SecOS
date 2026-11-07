# M52 — Phase D: the BKL leaves the syscalls (`integ/bkl-off`)

Phase D of `docs/BKL_REMOVAL_PLAN.md`, on top of phases A–C (M50, M51,
integrated on `integ/bkl` at c56c1b8). Each step clears bits in
`syscall_bkl_mask[]` (`kernel/bkl.c`) or removes one BKL call site, and
leaves the system working in both modes:

- `bkl=on` takes the BKL for every syscall, fault, teardown and
  kernel-shell command, exactly as in M48. It is the fallback.
- `bkl=off` takes it only for the syscalls whose bit is still set, and at
  the call sites that are left.

## Build switch

`make iso BKL_DEFAULT=off|on` picks the mode used when the command line has
neither boot word (`-DSECOS_BKL_DEFAULT_OFF`, rebuilt through a stamp like
`KEYMAP`). Make reads it from the environment too, so
`BKL_DEFAULT=off tools/selftest.sh` runs the whole suite in the other mode.
The boot line always says which mode is active, where it came from and how
many syscalls still take the BKL:

```
[BKL] kernel lock mode: bkl=off (default), syscalls still under the BKL: 0x…/0x100
```

## How each step was audited

Before a bit is cleared, the call graph of the syscall is walked for state
that another CPU can reach: globals, another process's fields (signal
posting, procfs, the reaper, the kernel shell, wake-ups), check-then-act
sequences on names, and user copies under a lock. Each finding is fixed
or the bit stays set. The fixes below are the bugs the BKL used to hide;
several were reachable even with `bkl=on`, from interrupt context (the
keyboard IRQ posts Ctrl-C/Ctrl-Z to the foreground group without the
BKL).

## D1 — process, signal, socket, pipe, memory and cred syscalls

Off the BKL with `bkl=off`: yield, getpid/getppid, getticks, sleep and
nanosleep, clock_gettime/settime, uname, sysinfo, umask, syslog, prctl,
sched_getaffinity, sigaction/sigreturn/sigprocmask/sigsuspend, kill,
setpgid/getpgid/setsid/getsid, wait/waitany, reboot, the driver syscall,
every socket syscall and netctl, pipe, msg_send/msg_recv, mmap/munmap/
mprotect/brk, the cred syscalls 100–109, and close/dup/dup2/fcntl. The
mask is built in `kernel/bkl.c` from one list per step.

Why they are safe: process-local fields are written only by the task
itself (no threads); cross-process readers and writers take `proc_lock`
(table, refs, credentials, pgid/sid after the fix below) or the owner's
`mm_lock` (VMAs, page tables, fd table, cwd); pipes, IPC, ptys, input and
the network stack have their own locks; closing a file fd calls no
filesystem (only pty/input/pipe/socket objects hold references).

Bugs found and fixed:

- **Half-built processes could be woken** (`66c7a2d`). A process is in
  the table before it has a kernel stack and a trapframe, and the kernel
  shell holds pipeline stages while it wires their fds. Both used
  `PROC_BLOCKED`, so a signal to the group (Ctrl-C from the keyboard IRQ,
  even with `bkl=on`; `kill()` from any CPU once it left the BKL) made it
  READY with `kstack_top == 0`. New state `PROC_EMBRYO` that no wake-up
  knows and no CPU picks. A process that fails after `proc_add` now waits
  for references taken meanwhile before it is freed.
- **waitpid could leave a zombie uncollected** (`729b73e`). The exit path
  counted a delivered status only when its own CAS made the waiter READY;
  a concurrent `signal_post` winning that CAS left the zombie uncollected,
  and a later `waitpid(-1)` reported it twice.
- **setpgid/setsid checked and stored without a lock**: a parent's setpgid
  could interleave with the child's setsid (a session leader whose pgid
  is not its pid). Both now check and store in one `proc_lock` section.
- **A SIGCONT or SIGKILL could be lost against a stop** (also with
  `bkl=on`): posted between the dequeue of the stop signal and the
  STOPPED store, it saw the task RUNNING. Both sides now re-check after
  their store (seq_cst), and the CAS resumes the task once.
- `sigreturn`'s fxsave probe buffer was static (shared by all CPUs).
- The umount busy check read other processes' fd tables without their
  `mm_lock` (`af01181`); the kernel shell wrote a held child's fds
  without it.
- `power_final` (the last phase of reboot/poweroff) takes the BKL and
  never releases it, parking the kernel shell and any syscall still under
  the BKL before the final sync (`2c10e49`).

## D2 — user page faults

With `bkl=on` a user-address fault runs under the BKL as before. With
`bkl=off` it takes no lock of its own (`714c0df`): the VMA lookup and the
PTE install run under the owner's `mm_lock`, the frame fill goes through
the page cache and the FS locks (`vma_fault_in_live`, B1/B2), COW under
`mm_lock` (the copy source is a private frame; frame refcounts are
atomic under `pmm_lock`). The fault handler now re-enables interrupts
while it handles a fault whose context had them on (ring 3 always, a
syscall's user copy too), so a fault that waits for an FS lock or a disk
keeps taking ticks and TLB shootdowns. `#PF` has no IST stack and ring 0
is not preemptible, so a nested fault or interrupt is fine; CR2 is
written back after an unsuccessful handling so the report shows the
right address.

## D3 — file syscalls

Off the BKL with `bkl=off`: open, lseek, stat/lstat/fstat, chmod/chown/
utimes, readlink/symlink, getdents, create, mkdir, unlink, chdir/getcwd,
statfs, sync, rename, link, truncate, rmdir, access. Every filesystem op
already ran under its driver's instance lock (M51 C2–C4) and namespace
changes under `vfs_ns_lock` with unmount draining (C1); what the BKL still
hid were check-then-act sequences in the syscall layer, which a second CPU
can now get between:

- **open vs umount** (`4e1ae5c`). The open path looked up with a mount ref,
  dropped it, and installed the fd later, so an umount could drain, find
  no fd and detach the filesystem the new fd pointed into.
  `vfs_lookup_pin()`/`vfs_unpin()` keep the mount's ref; open holds it from
  the lookup to the fd install (the busy check runs after the drain, so it
  sees the fd). execve and spawn pin the mount while they read the image.
- **open(O_TRUNC) truncated the wrong file** (security). It checked
  write access on the looked-up inode, then truncated *by name* and looked
  the name up again: a rename of a symlink to `/etc/shadow` over the
  caller's own file in between gave a writable fd on `/etc/shadow`.
  truncate(path) had the same shape. Both now truncate the pinned inode
  they checked.
- **Inode-keyed operations** (`feat/fs-inode-ops`, merged in `acec3c6`,
  done by a helper agent on this API): new optional ops `ftruncate(ino)`,
  `fsetattr(ino)`, `create_attr(path, attr)`, `mkdir_attr(path, attr)`,
  each one driver critical section (one JBD2 transaction on ext4), in
  ext2/3/4, ramfs, FAT32 and exFAT, with VFS fallbacks for the others.
  `vfs_ftruncate` was `ops->truncate(ino->path)`, and `ino->path` is stale
  after a rename (`mv log log.1; ftruncate(fd)` truncated the new `log`).
  ext2 now marks a slot dead when its inode is freed (a reused inode number
  gets a fresh slot; an fd on the deleted file gets EIO instead of another
  file's data), and ramfs recycles a dead inode only when no fd or mapping
  holds it.
- **New files were briefly root-owned 0644** (security, `38620c7`):
  created by `vfs_create`, then fixed up by `cred_apply_new`. Another user
  could open the file in between and keep the fd (`umask 077; echo secret
  > f`). open(O_CREAT), create and mkdir compute the attributes first
  (`cred_new_attr`) and create with them (`vfs_create_attr`,
  `vfs_mkdir_attr`).
- **chmod/chown/utimes** checked the owner on one lookup and changed the
  name again; they now change the pinned inode they checked
  (`vfs_fsetattr`), and fchmod/fchown/futimes act on the fd's inode
  instead of the path it was opened by.
- **pty slave lookups** (`fedfcca`): `/dev/pts/N` handed out `&p->sino`
  after dropping `g_pty_lock`, and a hangup could `kfree` the pty before
  the VFS wrote `ino->ops` into it (a write into freed heap). Ptys now
  live in a static pool, never freed; the open hook re-checks the caller's
  access against the live slave under the lock (the slot may have been
  reused by another user's pty), and chmod/chown of `/pts/N` are applied
  to the live slave under the lock by its owner or root.
- **O_CREAT race**: the loser of two concurrent creates of one name got
  -ENOENT; it now opens the winner's file (permission checked as for an
  existing file, -EEXIST with O_EXCL).
- **spawn** (`3750230`) took the set-id mode from one lookup and the image
  from another, so a rename could pair file A's set-id bits with file B's
  contents; one pinned inode now provides both. Its argv bounce buffer was
  static (shared by every CPU).
- The kernel shell's `mountdev`/`ext2mount` now run under `vfs_ns_lock`,
  its `rf*` debug commands copy ramfs entries under the ramfs lock.

Left as they are (documented risks, see the end): unlink/rmdir/rename/link
check permissions on one lookup and act by name, which needs the caller to
have write access to the directory (sticky directories narrow it to its
own entries); symlink creation sets the owner after the link exists
(symlink modes are never checked).

## D4 — read, write, ioctl, poll

They dispatch on the fd type, so they left the BKL last among the I/O
syscalls (`20731f7`): files (page cache, the driver under its lock, user
copies through a bounce buffer outside every lock), pipes, sockets, ptys,
the console line discipline (`tty_con_lock`; its waits hold no klock, and
`tty_wait_irq`'s `bkl_suspend` is a no-op without the BKL), devfs (no
devfs klock on the console paths; raw block nodes under the base disk's
lock), evdev, fb0, procfs, sysfs. `/dev/random` used to hold the csprng's
irqsave lock for the whole request (64 KiB = 2048 SHA-256 blocks with
interrupts off); it now serves 256-byte chunks, each its own request with
its own rekey (`8a241e5`).

## D5 — fork, spawn, execve, mount, umount

Process creation runs on `proc_lock` (pid, table, home CPU),
`kstack_lock`, the new space's `mm_lock` and the fd objects' own locks; a
new process is EMBRYO until it is built (D1); spawn and execve read, check
and set-id one pinned inode into a per-call buffer. mount/umount run under
`vfs_ns_lock`, umount drains in-flight operations before its busy check
(`86465e3`).

## D6 — teardown and the kernel shell

Reap, exit, kill and stop take the BKL only with `bkl=on` (`e7183f7`).
Everything they touch has its own lock: fd objects (a file fd holds no
FS reference), driver bindings, the address space and frames, the process
table, re-parenting and parent wake-ups, the console handoff. A dying task
is RUNNING and on its CPU, so no wake-up CAS can move it; the reaper only
takes detached zombies. With `bkl=off` the zombie reap at the top of every
fork/spawn and in every idle loop no longer waits for the shell or for
each other. Exit's bit is cleared; after D6 **no syscall, user fault or
teardown takes the BKL with `bkl=off`**. The bits still set in the mask
are numbers no syscall uses (they return -ENOSYS at once): they stay set so
a syscall added later starts under the BKL until it is audited.
`g_bkl_keep` (`kernel/bkl.c`) is the place to park a syscall back under
the BKL; it is empty.

**Decision: the kernel shell keeps the BKL** around every command, in
both modes (`07ceb87`). With `bkl=off` nothing else takes it, so it is in
effect the shell's own lock: it serializes the shell's globals (cwd, jobs,
pager, the 2 MB job spawn buffer) and the final poweroff phase, and tasks
on other CPUs never wait for it. A new klock would not do: the shell waits
(pager, child waits, the network commands' `cpu_wait_irq`) need the one
klock that may be held across a wait and suspended (`bkl_suspend`), and as
K0 it nests outside every lock the commands take. What the shell shares
with tasks goes through their locks.

## The deadlock the stress found: `mm_lock` vs `proc_lock` through the tick

The first proc-race run (`prd8`, -smp 8, lockdep) after D3 stopped within
seconds: lockdep's watchdog printed CPU 2 holding `procfs` + `proc` and
spinning on a process's `mm`, the CPU that held that `mm` spinning on
`proc` from its timer interrupt, and the other CPUs queued on `proc`.

- procfs (`snap_cb`), sysfs, the umount busy check and the ramfs recycle
  check take a process's `mm_lock` *under* `proc_lock` (S30 → S35);
- the timer tick (sleepers, console readers, load average) and the
  keyboard IRQ (Ctrl-C/Ctrl-Z) take `proc_lock` in interrupt context;
- `mm_lock` was deliberately not irqsave (B1: "no interrupt handler takes
  it"), so its holder could take the tick.

CPU A holds `mm` with IF on → tick → spins on `proc`; CPU B holds `proc` →
spins on A's `mm`. With the BKL the procfs readers and the mm-editing
syscalls were serialized, so B never waited while A held `mm` in a
syscall. Lockdep checks order per interrupt frame, so it cannot see this
cross-frame cycle (Linux's lockdep would: an IRQ-safe lock depending on an
IRQ-unsafe one). Fix (`dc3b8d4`): `mm_lock` is irqsave (and so is the
fault path's direct use); `mm_relax()` still opens an interrupt window in
long unmap/protect loops. The cost is that a long fork walk delays the
tick on its CPU. I checked the other non-irqsave spinlocks (`g_proc_lock`,
`g_sysfs_lock`, S15): nobody waits for them while holding a lock that an
IRQ takes (that would be an S30 → S15 inversion, which lockdep reports),
and klocks are never taken under a spinlock, so no other cycle of this
shape exists.

## Test tooling changes

- `make bklstress` (`tools/bkl_stress.sh`): one lockdep image (heap
  poisoning, 2 s stuck watchdog) in the chosen default mode; runs, a few
  at a time, the SMP stress (-smp 8), proc race (-smp 8), network stress
  with ssh sessions (-smp 4, e1000e MSI-X), TTY/pty/pipeline stress,
  parallel exec (500 × `bash -c true`, -smp 8), fsstress on the ext4 root
  (-smp 4 and 8, host `e2fsck -fn`), on ramfs `/tmp`, on a FAT32 and an
  exFAT data disk (with a 40-round mount/umount race; host `fsck.vfat -n`,
  `fsck.exfat -n` + `deepcheck`), and the kill-9 crash/journal-replay loop.
- `fs_stress_drive.py --data vfat|exfat --mount-race N`. The kernel drives
  one virtio disk and probes AHCI only without one, so with a data disk
  both disks are SATA on q35 (root `sda`, data `sdb`).
- `fsstress` accepted only lower-case slot names; FAT32 lists `F0..F7`.
- `locktests.sh` (and the new script) evaluated `$?` after a command
  substitution, so a failed run printed `[PASS] … passed`. Fixed.
- All serial drivers waited for the output to *end* with bash's `#`. init
  prints its console lines in parallel, and with syscalls off the BKL one
  can land right after the first prompt (seen twice): the drivers now
  send a CR every 5 s once bash has printed a prompt.
- `/proc/meminfo` gains `Slab` (frames the kernel heap holds from the PMM:
  it never gives them back) and `SUnreclaim` (bytes in heap blocks), from
  O(1) counters. The exec, proc-race and network drivers check `MemFree +
  Slab` (frames leaked outside the heap) and `SUnreclaim` (heap blocks
  leaked) instead of `MemFree` alone.

### NETSMP `r2-memfree-stable`

The marginal failure (MemFree −2280 kB between rounds vs a 2048 kB limit)
is heap growth, not a leak. With the new fields one run shows it directly:
MemFree fell 4576 kB from the base sample to round 1 while Slab rose
15396 → 19956 kB (+4560 kB) and SUnreclaim stayed at 12.8 MB (+15 kB);
from round 1 to round 2 nothing moved. More concurrent syscalls raise the
heap's peak (TCP buffers of more simultaneous connections), and the heap
keeps every frame it took. The same holds for the exec stress at -smp 8
(−1232 kB MemFree after D1, `MemFree + Slab` steady after the change).
The check now looks at `MemFree + Slab` and `SUnreclaim` with the old
limits; a real per-connection leak (128 KB of TCP buffers each, ~100
connections a round) still exceeds them by far.

## D7 — default `bkl=off`

`BKL_DEFAULT ?= off` (`20ce2c3`). The third GRUB entry boots the other
mode: with the default build it is **"SecOS x64 (fallback: big kernel
lock, bkl=on)"** (a `BKL_DEFAULT=on` build offers "no big kernel lock:
bkl=off" there instead). `bkl=on` on the MB2 command line or in the UEFI
load options works as before. The kernel code of `bkl=on` is unchanged:
every syscall, user fault, teardown and kernel-shell command takes the
BKL, exactly as in M48. `make lockdep-check` now boots the build default
plus the other mode (`alt4`).

## Scaling report (plan §7.7)

`tools/bkl_bench.py`, normal build (no lockdep), fresh ext4 root on
virtio, one image booted in each mode; both modes of one `-smp` ran side
by side on the same host (24 cores, QEMU TCG, other agents' guests on the
host too, so take ±15%). exec = 4 parallel loops of `bash -c true`; pipe =
4 parallel loops of `seq 1 2000 | cat | wc -l`; fsstress = 6 workers × 100
iterations on the ext4 root (each iteration is several file operations).

| -smp | mode | exec/s | pipelines/s | fsstress iters/s (ext4) |
|---:|---|---:|---:|---:|
| 1 | on  | 8.13 | 4.58 | 6.72 |
| 1 | off | 8.29 | 4.64 | 6.75 |
| 4 | on  | 3.06 | 1.73 | 5.78 |
| 4 | off | **11.44** | **5.48** | 6.95 |
| 8 | on  | 1.68 | 0.68 | 6.65 |
| 8 | off | **9.00** | **3.79** | 5.64 |

- With the BKL, adding CPUs made exec and pipelines **slower** (8.1 → 1.7
  execs/s from 1 to 8 CPUs): every syscall of every task waited for one
  lock, and the waiters burned the CPUs the holder could have used.
  `/proc/lockstat` at -smp 8, bkl=on: `bkl` 753290 acquisitions, 721502
  contended (96 %), longest wait 1.16 s.
- Without it, exec/pipelines are 3.7× (-smp 4) and 5.4–5.6× (-smp 8)
  faster than with it, and -smp 4 beats -smp 1 by 1.2–1.4×. The guest is
  TCG on a loaded host, so -smp 8 does not scale further (8 vCPU threads
  competing with the other guests); the kernel no longer serializes.
- fsstress on ext4 does not scale in either mode: one lock per volume is
  held for each whole operation including the synchronous JBD2 commit
  (M51 C3). bkl=off, -smp 8: `ext2` 47389 acquisitions, 40018 contended
  (84 %), longest wait 0.2 s — **the next bottleneck** for file I/O; the
  page cache absorbs repeated reads. exFAT on SATA in the lockdep stress
  ran the same fsstress load ~25× faster than ext4 (7–8 s vs ~200 s; no
  journal).
- ramfs (`fs_stress_drive.py --ramroot`, lockdep build, bkl=off, 3 workers
  × 1500 iterations): 30–33 iters/s at -smp 8.
- Spinlock statistics (`heap_lock`, `proc_lock`, ...) need a lockdep
  build; `heap_lock` (global, first-fit walk) remains the bottleneck
  named by M51 C6 for parallel exec.

## Found at the end: ext2 slots of unlinked files were never reused

The inode-keyed ops mark an ext2 VFS slot dead when its on-disk inode is
freed (so an fd on a deleted file cannot reach the file that gets the
number next). Dead slots were never bound again, so every create+unlink
consumed a slot for the life of the mount, and after 32768 of them
(`E2_VCHUNKS_MAX * E2_VCHUNK`) new files could not be looked up — a few
hours of a log-rotating service, or ten fsstress runs. `7a0fb23`: before a
new chunk is allocated (once per 64 fresh slots) and when the pool is
full, a dead slot that no fd or file mapping holds
(`process_inode_in_use()`, fd tables and VMAs under each owner's
`mm_lock`) is bound again and its cached pages are dropped once more.

## What still takes the BKL

- With `bkl=off`: **no syscall** (every defined number is in a D list),
  no user fault, no teardown. The kernel shell's commands (its own lock in
  effect), `power_final` (parks shell commands before the last sync), and
  syscall numbers that do not exist (they return -ENOSYS; kept set so a
  future syscall starts under the BKL until audited). `g_bkl_keep` is
  empty.
- With `bkl=on`: everything, as in M48.

## Verification

Every run below used a private `/tmp` (`unshare -rm`), random host ports.

| Run | Image | Result |
|---|---|---|
| `make bklstress` after D1 (BKL on file I/O) | lockdep, bkl=off | 23/23 (exec memfree flagged heap growth, see NETSMP above; FAT/exFAT runs had not reached the disk, fixed) |
| bklstress after D3 | lockdep, bkl=off | **found** the `mm_lock`/`proc_lock` deadlock (prd8) and the prompt-race in the drivers |
| bklstress after D6 + `dc3b8d4` | lockdep, bkl=off | **23/23**, 0 lockdep reports |
| ramfs root fsstress (3 × 1500, -smp 4 and 8) | lockdep, bkl=off | PASS, 0 reports |
| `make lockdep-check` (default off, alt on) | lockdep | **18/18** |
| `make locktests` (9 forced-concurrency tests incl. the new `fsops`) | lockdep, bkl=off | **18/18** |
| full `tools/selftest.sh`, pre-D7 default (bkl=on), at `07ceb87` | normal | 1267 passed, 1 failed: the new exec `heap-in-use-steady` check saw +1.7 MB (an unreaped bash zombie's image); the driver now takes the best of three samples (`536e024`) |
| full `tools/selftest.sh`, D7 default (bkl=off) | normal | see below |
| soak: `tools/soak.sh` + syscall fuzzer, -smp 8, 5 seeds × 2500 commands, 32 min | normal, bkl=off | **5/5 PASS**, no kernel fault |
| bklstress subset on the final tip (fsd4 esd8 fsc4 fsr4 prd8 fsx4) | lockdep, bkl=off | see below |

`-Wall -Wextra`: 35 warnings at every commit, as at the base.
