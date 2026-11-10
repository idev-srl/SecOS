# U9 — threads (`feat/threads`)

Userland plan §3 U9, design §2 D5. SecOS runs POSIX threads: musl's
`pthread_create` makes a kernel thread, and everything a threaded program
needs underneath (futex, per-thread signals, cancellation, `/proc/<pid>/task`)
is there. U11 (OpenSSL, curl's threaded resolver), CPython and cmake can use
threads; the earlier single-threaded builds keep working unchanged.

## Quick start

```
tools/secos-cc -O2 -o prog prog.c          # pthreads need nothing extra (musl)
thrstress all 8                            # /bin/thrstress: the pthread self-test
smptest threads 8 10                       # raw-thread SMP stress (futex + shared mm)
cat /proc/<pid>/status | grep Threads      # /proc/<pid>/task/<tid>/... per thread
tools/threads_test.sh [--lockdep] [--smp "1 2 4"] [--iso F] [--keep] [--no-nothreads]
make iso KERNEL_CMDLINE=nothreads          # fallback: clone(CLONE_THREAD) refused
```

## Object model (D5 option T2)

`process_t` stays the schedulable task, one per thread: kernel stack and slot,
trapframe, FPU image, state and every wait field, the signal mask and the
thread-directed pending set, the FS base, `clear_child_tid`, the futex wait
state. What the threads of a process share moved into two refcounted objects
(`kernel/process.h`, `kernel/thread.c`):

| Object | Contents | Lifetime |
|---|---|---|
| `proc_mm_t` | page tables (`space`), VMA set, pinned ELF image, brk/mmap state, `lock` (= the old `mm_lock`, S35), `map_lock` (K13) | one ref per task; the last `mm_put` destroys the space and the image |
| `thread_group_t` | `tgid`, `nr_live`, `dead`, group exit (`group_exit`, `group_code`), `exec_tid`, group stop (`stop_active`, `stop_done`, `nr_stopped`, `stop_sig`), `shared_pending`, the fd table + `fd_lock` (K12), `cwd`, `umask`, `handler[64]`, `restorer` | one ref per task; the fd objects are dropped when the last thread exits, the struct with the last ref |

`process_t.fds`, `.cwd` and `.sig_handler` are pointers into the group, so the
~150 `c->fds[fd]` sites (and U10's new fd types) compile unchanged;
`p->vmas`/`brk_*`/`mmap_next`/`image` became `p->mm->...`; `p->space` stays as
a per-task copy for the scheduler (only an exec changes it, and an exec is
single-threaded). fork and every image creation make new objects; `clone`
with the thread flags shares both. Only two flag combinations exist (thread or
fork), so two objects (not the plan's three) are enough; splitting `files_t`
out again for `CLONE_VM`-only users (vfork) is mechanical.

Process-wide values that stay per-task copies are kept identical in every
thread: credentials (`process_set_cred` writes all threads under proc_lock;
`access()` uses `process_set_cred_task` for its temporary real-id check),
pgid (setpgid), no_new_privs (prctl), caps/manifest/mem_limit (only an exec
changes them). sid and the controlling tty are per-task references
(`pty_ctty_ref` per thread); acquiring or dropping them (`setsid`,
`TIOCSCTTY`, `TIOCNOTTY`) is refused with EPERM while a process has more than
one thread (daemons do it before starting threads).

The KD_GRAPHICS owner (fbdev) and the session-leader hangup now use the tgid.

### Identity

`pid` is the thread id (unique, from the same counter), `tgid` the process.
getpid = tgid, gettid (512+186), set_tid_address returns the tid. ppid,
spawner_pid, pgid and sid hold tgids. `/proc` lists processes (leaders);
`/proc/<pid>/task/<tid>/` is a pid directory of each thread (`/proc/<tid>`
also resolves, hidden from the listing, as on Linux). A leader's
`/proc/<pid>/stat` and `status` sum utime/stime/nvcsw over its threads;
`Threads:` and `num_threads` are the live threads. `/proc/self` is the tgid.

## Kernel calls

| Number | Call | Notes |
|---|---|---|
| 568 = 512+56 | `clone(flags, newsp, ptid, ctid, tls)` | required `CLONE_VM\|FS\|FILES\|SIGHAND\|THREAD`, optional `SYSVSEM\|SETTLS\|PARENT_SETTID\|CHILD_CLEARTID\|DETACHED\|CHILD_SETTID`, anything else `-EINVAL`; `flags == SIGCHLD, newsp == 0` = fork; `-EPERM` for a ring-3 driver; `-EINVAL` under `nothreads`; `-EAGAIN` while the group exits / execs |
| 572 = 512+60 | `exit(status)` | this thread only |
| 698 = 512+186 | `gettid()` | |
| 712 = 512+200 | `tkill(tid, sig)` | |
| 714 = 512+202 | `futex(uaddr, op, val, timeout\|val2, uaddr2, val3)` | below |
| 746 = 512+234 | `tgkill(tgid, tid, sig)` | `-ESRCH` if tid is not in tgid |
| 785 = 512+273 | `set_robust_list(head, 24)` | stored, not walked at exit |
| 786 = 512+274 | `get_robust_list(tid, *head, *len)` | own threads only |

clone/exit/futex are dispatched in `syscall_trap.c` (they need the trapframe),
the rest in `syscall_lx.c`; all are BKL-free (`lx_lockfree`).

### Exit

`exit_current()` (sched.c) is the common path of SYS_EXIT / exit_group / exit
/ a fatal signal:
1. a group exit marks `group_exit`/`group_code` and SIGKILLs every other live
   thread (`thread_kill_others`, `thread_killed` so they exit as threads);
2. `CLONE_CHILD_CLEARTID`: zero the word, futex-wake one waiter (this is
   what releases musl's thread-list lock and `pthread_join`); off any futex;
3. under proc_lock `nr_live--`; a non-leader is `collected` at once;
4. the LAST thread ends the process (`exit_process_end`): the group status
   (`group_code`, else the leader's own exit status) becomes the leader's
   `exit_code`, the fds are closed, `tg->dead` set (only now may the parent
   collect the leader, or the reaper free it), children go to init, SIGCHLD
   and the waiters are woken for the tgid. A thread that is not the last only
   drops its ctty reference.

The reaper skips a leader whose group is not dead; wait/waitany/has_children
consider leaders only. A leader that called exit(2) first stays a zombie until
the last thread is gone (Linux).

### execve and fork from a thread

`execve` in a multi-threaded process (`thread_exec_dethread`) sets `exec_tid`,
SIGKILLs the others and re-runs (one tick at a time) until it is alone; a
non-leader then takes over the leader's pid (the old leader zombie gets the
thread's tid and is reaped as a thread). The others are killed before the new
image is loaded, so a failed execve from a thread returns its error to a
process that has lost its other threads (Linux kills them at the point of no
return, after loading). `fork` copies only the caller (one thread, new mm and
group, fd objects re-referenced under the parent's fd_lock).

### futex (`kernel/futex.c`)

256-bucket hash of waiting tasks under one spinlock (`LK_S28_FUTEX`, nothing
nested inside). Keys: `(mm, uaddr)` for private mappings, with or without
`FUTEX_PRIVATE_FLAG` (Linux keys a shared futex on private anonymous memory
the same way); `(phys | 1, 0)` for `VMA_TYPE_PHYS` (device memory, the only
memory two address spaces share today — U4a's anonymous `MAP_SHARED` and
writable shared file maps must key by frame / inode+offset).

WAIT uses the M16 block + re-run model: the first run queues the task, then
reads the word (no lock held, a user copy): a wake after the read finds us
queued, a wake before it changed the word first (every correct futex user
does), so we see the new value (`-EAGAIN`). The waker dequeues, sets
`futex_woken`, and makes the task READY under proc_lock
(`sched_wake_task_sync`: the CAS cannot fall between the blocker's
wake-sequence check and its BLOCKED store); a wake racing the block bumps the
wake sequence and the block becomes a re-run. The re-run returns 0 when
woken, `-ETIMEDOUT` past the deadline, `-EINTR` for a signal (a stop signal
stops with the call rewound and resumes the wait); a spurious re-run re-reads
the word (changed: 0, a permitted spurious wakeup; same: block again).
Waiters are woken FIFO per key, so a new waiter never takes the wake an older
sleeper needed. Timeouts: relative (WAIT), absolute MONOTONIC or REALTIME
(WAIT_BITSET + `FUTEX_CLOCK_REALTIME`, converted to monotonic once), 1 ms
tick granularity with the deadline checked in ns. CMP_REQUEUE compares the
word just before taking the lock (no user copy under a spinlock). Not
implemented: PI futexes, WAKE_OP, WAIT_REQUEUE_PI (`-ENOSYS`; musl uses PI
only for PI mutexes).

### Signals (`kernel/signal.c`)

- `NSIG` 64: signals 1..63 (bit n = signal n; Linux's 64 has no bit and is
  refused). The translation layer converts masks for all 63.
- `p->sig_pending` is thread-directed (tkill/tgkill, faults), the group's
  `shared_pending` process-directed. `signal_post()` keeps the pre-U9 path for
  a one-thread process; with several threads `signal_post_group()` pends the
  signal on the group and wakes a thread that does not block it (the target
  first, then one that is not asleep, then a sleeping one). Delivery takes
  the thread's own pending first, then the group's (an atomic fetch-and, so
  two threads never both take one signal).
- SIGKILL (also via tgkill) ends the group; SIGSTOP/TSTP/TTIN/TTOU at
  SIG_DFL start a group stop: every thread stops at its next return to ring 3
  (a sleeping one is woken for it and stops with its call rewound), and the
  leader reports the stop once all live threads are stopped. SIGCONT ends it,
  wakes them all and reports "continued" once.
- A fatal default action or a ring-3 fault ends the whole group.
- Signal frames now carry a Linux `siginfo_t` and `ucontext_t` (rsi, rdx for
  every handler, not only SA_SIGINFO ones); sigreturn takes registers, mask
  and FPU image from the ucontext, so a handler may change them (musl's
  cancellation moves the pc). si_code is SI_USER, si_pid 0 (no sender
  tracking yet). sigreturn also clears IOPL/NT/VM from the restored rflags
  (it only forced IF before: a forged frame could set IOPL 3, i.e. port I/O
  from ring 3).

### Shared fd table

The fd table had a single writer before U9; now any thread may close or dup
while another uses the fd:
- close / dup / dup2 / F_DUPFD and fork's copy run their read-modify-write
  and the object reference under the group's `fd_lock` (a K12 klock: the fd
  objects' own locks rank inside it); the entries are still published under
  `mm->lock` for procfs;
- a syscall working on an fd object (read, write, ioctl, fstat, lseek, fcntl,
  dup*, the socket calls) holds a reference on it for the call when the
  process has several threads (`thread_fd_pin`/`unpin`, dropped before the
  task blocks, stops or exits);
- poll's readiness scan runs under `fd_lock`.
A one-thread process takes none of these paths (no other thread can close).

### Address space

brk/mmap/munmap/mprotect serialize on `mm->map_lock` (K13): their page-table
loops drop `mm->lock` now and then (`mm_relax`), which was safe only because
the owner was the sole editor. Faults stay unlocked against them (the M50-B1
snapshot/recheck), with one fix (below). fork from a thread is covered by
`vmm_fork_space`'s shootdown of the parent's other CPUs (M48 `active_cpus`).

mmap now reuses holes: when the bump pointer reaches the end of the 3.75 GiB
arena, a first-fit search among the live VMAs places the mapping in a hole a
munmap left. musl gives every thread an 8 MiB stack (the `PT_GNU_STACK` size
`secos-cc` links), so a process exhausted the arena after ~450 thread
lifetimes. U4a replaces both with a real allocator.

Both per-process klocks are kept out of the `/proc/lockstat` registry
(`reg = 0xFF` at init): the registry keeps a pointer to each klock forever,
and these live in freed memory once their process is gone (found by the
gate: `ls /proc` stats `/proc/lockstat`, which followed a freed klock).

### Limits

`MAX_PROCESSES` = `M5_MAX_KSTACK_SLOTS` = 1024 tasks (threads count), the
kernel-stack region grew to 80 MiB of VA (72 KiB per slot, frames only while
in use), `VMA_MAX` 256 (two per thread stack: guard + stack), `/proc` lists
up to 512 processes. A per-user `RLIMIT_NPROC` and a dynamic table are U4a.

## musl (`user/musl-secos`)

| File | What |
|---|---|
| `src/thread/x86_64/clone.s` | SecOS clone (568) with the SecOS argument registers; the thread pops its argument and leaves with exit (572) |
| `src/thread/x86_64/__unmapself.s` | munmap (15) + exit (572) without touching the stack |
| `src/thread/x86_64/syscall_cp.s` | `__secos_cp_trap`: the cancellable kernel call with `__cp_begin`/`__cp_end`/`__cp_cancel` around it |
| `src/secos/syscall.c` | `__syscall_cp_asm` (C: translate, then trap through `__secos_cp_trap` for read, write, wait4, nanosleep, clock_nanosleep, poll/ppoll/select/pselect6, rt_sigsuspend, futex; a pending cancel is acted on at entry for the others); exit = thread, gettid, tkill, tgkill, futex, robust list, clone (fork form only: a raw clone returning on a new stack inside C cannot work), signals 1..63, SA_SIGINFO honoured, the wait4 stash locked |

musl's own `pthread_cancel.c` (the SIGCANCEL handler that checks the pc
against `[__cp_begin, __cp_end)`) is unmodified: the kernel's ucontext gives
it the pc. A thread blocked in a cancellable call is woken by SIGCANCEL, its
call returns `-EINTR` (pc == `__cp_end`), the handler re-pends SIGCANCEL with
it masked, and `__syscall_cp_c` cancels on `-EINTR`. Not honoured (partial
under `SECOS_SYSCALL_TRACE=1`): `SA_RESTART` (interrupted calls return EINTR;
musl's futex waits treat that as spurious), `SA_ONSTACK`, `SA_RESETHAND`,
`SA_NODEFER`; no `sigaltstack`.

## Bugs found in existing code

- **mm/vma.c**: a demand fault whose VMA changed between the snapshot and
  the install (an adjacent mmap extending it, mprotect splitting it) failed
  and killed the task. Impossible with one task per address space; with
  threads it killed musl's malloc under `-smp 2`. Now the access is retried.
- **mm/vmm.c**: a COW write fault on a page another thread had just copied
  found a writable non-COW PTE and killed the task (after a fork from a
  multi-threaded process). Now retried.
- **kernel/signal.c**: sigreturn let a forged frame set IOPL (and NT/VM) in
  the restored rflags: IOPL 3 grants `in`/`out` to ring 3. Cleared now.
- **kernel/sched.c**: `sched_wake_task()` made its CAS outside proc_lock;
  a blocker that read the wake sequence just before the waker's bump and
  stored BLOCKED just after the waker's state check slept with its condition
  met (signal wakes, the M52 stop/continue paths, udrv). The check and the
  CAS now run under proc_lock after the bump (callers hold nothing at or
  above S30; udrv's S25 event lock may be held, S25 -> S30 is in order).

## Deviations from the plan

- **Two shared objects**, not three (mm + group): only the thread and fork
  flag sets are accepted, so files and sighand always go together.
- **Commits**: steps 1–3 (thread groups, futex, per-thread signals) are one
  kernel commit; they depend on each other (CLONE_CHILD_CLEARTID needs the
  futex wake, a group exit needs thread-directed SIGKILL) and were gated
  together. musl (step 4) and step 5 are separate commits.
- **execve from a thread** kills the others before loading the new image
  (see above).
- **setsid / TIOCSCTTY / TIOCNOTTY** are refused in a multi-threaded process.
- **NSIG 64 = signals 1..63** (SIGRTMAX is 63, not 64).
- **libc-test pthread tests** are not added to a LIBCTEST pass list: LIBCTEST
  (U3) is not in this tree yet.
- **robust lists** are stored, not walked at thread exit.

## Gate THREADS (`tools/threads_test.sh`, in `tools/selftest.sh`)

Host: sysroot, both test programs build signed and warning-free. Guest
(CONSOLE=root, no root disk, binaries on an ext2 disk at /mnt, `-cpu max`:
SMAP + KASLR) at -smp 1, 2 and 4 (N = 8, 8, 16 worker threads): every
`thrraw` and `thrstress` test is a check (22 + 20), smptest threads/thrkill,
the shell checks (Threads: 9, 9 task entries, kill -9 -> 137, SIGINT ->
130), a second full run, no SUnreclaim growth (+0..1 kB) and no frame leak,
a clean debugcon; then a `nothreads` boot (pthread_create and raw clone
refused, bash and musl programs fine). `--lockdep` repeats it on a
`-DSECOS_LOCKDEP` image (selftest: `THREADS_LOCKDEP=1`).

## Tests run (at the end of U9)

| Run | Result |
|---|---|
| `tools/threads_test.sh` (-smp 1/2/4 + nothreads) | 169/169 |
| `tools/threads_test.sh --lockdep --smp 4 --no-nothreads` | 58/58, no lockdep report |
| selftest M16, M19, execve, M25, M30, M29-3, M48 gate (incl. smp_stress_drive -smp 4/8), PROCRACE (lockdep, -smp 4/8), M51 EXEC | 142/142 (the 4 M48 boot-marker checks failed once with two gate sets in parallel: the 40 s boot window; re-run alone: pass) |
| selftest bash regressions (command substitution, job control, bash over execve) | 37/37 |
| `tools/musl_base_test.sh` (MUSL-BASE) | 55/55 |
| `tools/nano_test.sh` (NANO) | 34/34 |
| `tools/persist_test.sh --crash-runs 3` (CONSOLE=root, as selftest runs it) | 30/30 |
| GUI compositor gate (`gui_wm_drive.py`) / TASKMGR (`--taskmgr`) | 57/57, 5 hostile clients cut / 34/34 |
| `tools/net_udrv_gate.sh` (M58 NET-UDRV) | 7/7 |
| `tools/smoke.sh --mb2` / `--uefi` | PASS / PASS |
| warnings (`make`, `make CFLAGS_EXTRA=-DSECOS_LOCKDEP`) | 0 / 0 (base 0 / 0) |

## Notes for the integrator

- **U10 (feat/net-posix)**: new fd kinds must be added to `fd_entry_ref` /
  `fd_entry_unref` (kernel/syscall.c) — the per-syscall fd pin, fork's copy
  and spawn's stdio take/drop references through them; their syscalls that
  take an fd in the first argument should be added to `fd_pin_call()`
  (kernel/thread.c) if their body uses the object through a raw pointer.
  epoll/eventfd/timerfd/accept/recv blocking waits are woken per task: with
  threads several tasks of one process may wait on one object, so every wake
  must wake all of them (or be a futex-like FIFO) and a wake that races the
  block must bump the wake sequence (`sched_wake_note`) or use
  `sched_wake_task_sync`. In musl, the blocking kernel call of new
  cancellation points (accept, connect, recv*, send*, epoll_wait/pwait) must
  go through `scp(cp, ...)` (src/secos/syscall.c) for pthread_cancel to
  interrupt them.
- **Mapping of U10 (feat/net-posix ee43af4) onto this design:**
  1. Wait state (`wait_pipe`, `sleep_until`, `ki.poll_wait`, `wait_tty`,
     `wait_pty`, U10's `lx_wait_rip`, `lx_deadline`) is already per thread:
     it stays in `process_t`, which is the thread. Nothing moves. Clone
     must reset the two new fields (`thread_clone()` resets every wait
     field next to `wait_pipe`: add them there); `process_exec` resets as
     for `sleep_until`. Wakes by object scan (`sched_wake_pipe`) and the
     `sleep_until` tick scan already reach every thread.
  2. `itimer_next_ns` / `itimer_int_ns` are per process: move them into
     `thread_group_t` (`p->tg->itimer_*`), zeroed in `tg_alloc` and
     `tg_copy_for_fork` (fork does not inherit timers); `lx_itimer_tick`
     should walk groups once (e.g. only the leader / first task of each
     `tg`) and post SIGALRM with `signal_post()` (process-directed: it goes
     to a thread that does not block it). `proc_fd_entry.gen` is a field
     of the entry, which now lives in `thread_group_t.fd[]`: it stays as is
     (`fd_alloc` still sets it under mm_lock).
  3. The anonfd kinds go into `fd_entry_ref`/`fd_entry_unref`; fork's copy
     (`process_fork`, under the parent's `fd_lock`), exec's cloexec loop and
     `process_release_pipes` kind 6 need no further change: since U9
     `process_release_pipes` runs only when the process ends (last live
     thread, `exit_process_end`) or from `process_destroy` of the last task
     of a group (`tg->refs <= 1`), i.e. on the last table reference.
  4. SIGPIPE in `maybe_sigpipe` / `unix_op_write`: the writer is the current
     thread: use `signal_post_thread(c, SIGPIPE)` (thread-directed, as on
     Linux). SO_PEERCRED: store `c->tgid` (getpid), not `c->pid` (the tid).
  5. The musl layer conflict is textual only (U9's block replaced the
     `clone/futex/set_robust_list` ENOSYS lines; U10 adds a pass-through
     block and `SYS_pause`). `pause` is a musl cancellation point: route it
     through `scp(cp, ...)` like `rt_sigsuspend`.
- `lx_lockfree()` (kernel/bkl.c) and `syscall_lx_dispatch()` /
  `src/secos/syscall.c` gained entries; U10 adds its own: take both.
- `kernel/sysroot.c` gains one `sf_bin("thrstress", ...)` line and an
  include; `tools/persist_test.sh` expects 77 system files (was 76).
- The fd table is still 32 entries (U4a: growable `files_t`, now a field of
  `thread_group_t`).

## Notes for U11/U13

- OpenSSL 3 threads: `pthread_once`, mutexes, rwlocks, TLS keys and
  `__thread` all work; build with threads (drop `no-threads`).
- curl's threaded resolver uses `pthread_create` + `pthread_join` (and
  `pthread_cancel` is not needed): fine.
- CPython needs `sem_timedwait`/`pthread_cond_timedwait` (CLOCK_MONOTONIC via
  `pthread_condattr_setclock`), `pthread_sigmask`, `pthread_kill`: all
  covered by thrstress. `sigaltstack` is still missing (Python's faulthandler
  uses it; it degrades).
- Threads per process: bounded by the 1024-task table and VMA_MAX 256 (≈120
  live threads with default stacks); a server with one thread per
  connection should use a pool until U4a.
