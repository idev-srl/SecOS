# M50 B1 — Process lifetime: references, `mm_lock`, locked credentials

Phase B, step B1 of the BKL removal (`docs/BKL_REMOVAL_PLAN.md` §4,
branch `feat/proc-lifetime`, on top of `feat/lock-core`). The BKL stays on
and the default boot behaves as before. This step makes the process table,
the address spaces and the credentials safe for the day the BKL no longer
serializes syscalls against each other and against the reaper. The lock API
is the one of `docs/devlog/M50-lock-core.md` ("For phases B–D").

## Process references (`kernel/process.{c,h}`)

`process_find_by_pid()` returned a bare pointer. Nothing stopped the reaper
on another CPU from `kfree`ing the process while a `kill()`, `waitpid()`,
procfs read or SIGCHLD delivery was still using it. It is gone. Every lookup
now holds a reference:

```c
process_t* process_get_by_pid(uint32_t pid);  /* +1 ref under proc_lock, NULL = no such pid */
process_t* process_get(process_t* p);         /* +1 ref on a process known to be alive      */
void       process_put(process_t* p);         /* NULL is fine                               */
process_t* process_get_last(void);            /* highest pid, with a reference              */
int        process_pid_state(uint32_t pid);   /* state, or -1: no pointer escapes           */
```

- `process_t.refs` is incremented under `proc_lock`, so a lookup cannot race
  the reaper's detach. It is decremented with a plain atomic. A put without a
  matching get prints `[PROC] BUG: process_put without a reference`.
- `process_reap_one()` skips a zombie whose `refs > 0`. Nothing is freed
  inside `process_put()`, because callers may hold the BKL or other locks
  there. The next `sched_reap_zombies()` frees the zombie: it runs on every
  fork/spawn syscall, in every AP idle loop and in the kernel shell's waits.
  A pinned zombie stays in the table meanwhile, so `process_count_all()` and
  the leak checks still see it.
- `process_get(p)` is valid only when the caller knows `p` is alive: `p` is
  the current task, or the call is made inside a `process_foreach` callback
  (which runs under `proc_lock`). Group signals use the second case:
  `kill -pgid`, `kill -1` and Ctrl-C collect their targets with a reference
  inside the callback and drop it after `signal_post()`.
- New processes start with `refs = 0`; fork resets it, and so does
  `sched_set_idle()` for the idle tasks. The M48 rule still applies: a
  creator must not touch the new process after `sched_make_ready()`
  (SYS_FORK/SYS_SPAWN read the pid first). The kernel shell keeps holding
  its jobs BLOCKED (`sh_hold_new`).
- Pointer-returning helpers became value-returning:
  `process_collect_zombie_child(ppid, &pid, &code)` collects under
  `proc_lock` (SYS_WAITANY used to set `collected` and then read
  `z->pid` — by then the reaper could already have freed `z`), and
  `process_take_jobctl_child()` returns a pid.
- `process_detach_unstarted(pid)`: the kernel shell's `elfunload` used to
  `process_destroy()` any pid, even a running one. It now frees only a
  process that is still `PROC_NEW`, is on no CPU and has no reference. The
  process is detached like a reaped zombie, and the caller ends with
  `process_reap_done()`.

Call sites converted (18 `process_find_by_pid`, 2 zombie/job-control
helpers, 3 `process_get_last`, 1 collect-then-post callback):

| Site | Now |
|---|---|
| `signal.c` `ksys_kill` | get/put; the credential check runs under `proc_lock` |
| `signal.c` `ksys_setpgid`, `ksys_getpgid`; `pty.c` `ksys_getsid` | get/put (pid 0 = self, no lookup) |
| `signal.c` `pg_cb` (group kill, Ctrl-C) | `process_get` in the callback, put after posting |
| `tty.c` `post_now` | get/put around `signal_post` |
| `sched.c` `notify_parent_sigchld` | get/put |
| `syscall_trap.c` SYS_WAIT | get, put on every path (before `sched_block_current`, which does not return) |
| `syscall_trap.c` SYS_WAITANY | `process_collect_zombie_child`, jobctl by pid |
| `syscall.c` `ksys_wait` | `process_pid_state` |
| `kernel.c` `init_kill_orphans` | get/put |
| `shell.c` `job_gc`, `job_wait_foreground` | `process_pid_state` |
| `shell.c` `shell_exec_foreground`, `kill`, `pinfo`, `drvreg`/`drvunreg` fallback | get/put |
| `shell.c` `elfunload` | `process_detach_unstarted` |

`process_foreach()` callbacks that only read fields still get a bare
pointer. That is safe because they run under `proc_lock`. They must not
keep the pointer after they return.

## `mm_lock` (`LK_S35_MM`, per process, irqsave)

`process_t.mm_lock` protects the VMA set, the user page tables of `space`,
and the `space`/`image`/`manifest` pointers. Initialize it with the new
`spin_init_class_dynamic()`, which is not entered in the `/proc/lockstat`
registry: that registry keeps a pointer to every lock it has seen, and a
`process_t` gets freed. Helpers: `mm_lock(p)`/`mm_unlock(p, fl)`, and
`mm_relax(p, &fl)`, which unlocks and relocks so interrupts can get in.
`MM_ASSERT_HELD(p)` checks the lock is held.

Who takes it:

- **The owner, for every edit:** `brk`, `mmap` (anon + file), `munmap`,
  `mprotect`, the framebuffer `mmap`, the `exec` swap of space + VMAs +
  image + manifest (this replaces the bare `cli`; the CR3 switch is inside
  it), the VMA copy and COW share of `fork` (on the parent), and the argv
  stack fill of a new process that is already in the table. Long PTE loops
  call `mm_relax` every 256 pages, so the BSP tick is not held off. This is
  only safe because the owner is the only writer of its own VMAs: SecOS has
  no threads.
- **The page fault:**
  - The COW check, copy and PTE swap run as one section. The source is a
    private frame, never the page cache.
  - Demand paging uses the new `vma_fault_in_live(space, vmas, &mm_lock,
    addr)`. It looks the VMA up and copies it under the lock, drops the
    lock, then allocates and fills the frame (this is where the page cache,
    and later a filesystem klock, can be entered), retakes the lock,
    re-checks that the VMA is unchanged, and installs the PTE. A page that
    was already installed counts as success.
  - `vma_fault_in()` remains as the unlocked primitive. Use it for a space
    nobody else can see (creation, exec staging), or under `mm_lock` for
    anonymous or device pages.
- **Cross-process readers:** procfs `snap_cb` (`vsz`, `vmm_count_user_pages`,
  brk, the `maps` VMA copy; order `proc_lock` S30 → `mm_lock` S35), umount's
  busy check of file mappings, and the kernel shell's `pinfo` manifest copy.

Rules for the next phases:

- Never hold `mm_lock` across a page-cache or filesystem call, a user copy,
  or anything that takes `proc_lock` or a lower level (S15 driver_if, S10
  tty, any klock).
- Inside it is fine: PMM/heap allocation, `vmm_map/unmap/protect_in_space`
  and TLB shootdowns (waiters spin in `cpu_relax_poll` → `tlb_poll`).
- `vmm_unmap_in_space()` and `vmm_protect_in_space()` take a space, not a
  process, so they cannot assert the lock. Their callers on a live space
  hold it; teardown (`process_destroy` → `elf_unload_process`,
  `vmm_space_destroy`) runs on a process that is detached and unreferenced.
- Exec frees the old space after the swap, outside the lock. Once `p->space`
  points to the new space, nothing can reach the old one.
- `driver_remove_all_bindings()` moved before the exec lock section,
  because `driver_if_lock` (S15) ranks below `mm_lock` (S35).

## Credentials (`kernel/cred.c`)

A writer builds the new set on a copy and publishes it with
`process_set_cred(p, &cr)`, which takes `proc_lock`. This covers the set*id
syscalls, the set-id exec transition, the spawner's credentials copied into
a spawned child, and the real-id swap of `access()`. Cross-process readers
(`cred_may_signal` in `pg_cb` and `ksys_kill`, `cred_may_inspect` in
`snap_cb`) run under `proc_lock`. The owner reads its own credentials
without the lock, which is consistent because only the owner writes them.
One semantic remains: during an `access()` check another process can see
the real ids as the effective ones. This is like the Linux access override,
except that Linux keeps it private to the task.

## `driver_if_lock`, cap audit head

- `driver_if_lock` (`LK_S15_SUBSYS`, irqsave) protects the binding table,
  the rate-limit table, the shadow registers and the Driver Space audit
  ring. The device table is written once at init and read without the
  lock.
- `cap_audit()` claims its slot with `__atomic_fetch_add` on the ring head
  and stores the record's `seq` last. `cap_audit_dump()` and
  `cap_audit_total()` read the head once.

## Found by the stress: procfs inode slots recycled under an open fd

procfs hands out `vfs_inode_t` slots from a 64-entry ring. A slot that an
open fd still pointed at could be reused by another lookup. A later `read()`
on that fd then returned the other node's contents. The stress saw this at
-smp 4: `/proc/21/status` read as `/proc/loadavg`, and `/proc/1501/stat` as
a status file. This is a pre-existing bug that the BKL does not cover,
because open and read are separate syscalls.

Fix: `pool_get()` skips the slots referenced by any process's fd table. The
scan runs under `proc_lock` (S15 → S30). A slot is recycled only when all
64 are open.

## Proc-race stress (`smptest procrace` + `tools/proc_race_drive.py`)

`/bin/smptest procrace [secs] [readers] [churners]` runs two kinds of
workers:

- **Readers** pick random pids around the live range (the upper bound is the
  last pid in `/proc/loadavg`). For each pid they do `kill(pid, 0)` and read
  `/proc/<pid>/status`, `maps` and `stat`, checking that the content has the
  right type.
- **Churners** fork short-lived workers. Each worker maps 16 pages, touches
  them, does an `mprotect` split and an `munmap` hole, then unmaps all,
  grows and shrinks the heap by 64 KiB, and sometimes leaves an orphan
  grandchild for init. About one worker in four is SIGKILLed while running.
  Every wait status is checked.

`tools/proc_race_drive.py` boots a CONSOLE=root image over serial and runs:

1. a warm-up;
2. a bash variant: `kill -0` and `cat /proc/<pid>/{status,maps}` against
   bursts of subshells, plus a `/proc/[0-9]*/status` sweep;
3. N measured runs of 8 readers × 4 churners, sampling MemFree after each.
   The samples must stay within 1 MiB of each other.

The debugcon log must be free of `[LOCKDEP]`, `[LOCK] BUG`, heap corruption,
`[PROC] BUG`, panic and `[EXC]`.

The stress is hooked in two places:

- the selftest **PROCRACE gate**: a `-DSECOS_LOCKDEP` image (0x6B poisoning)
  at -smp 4 and -smp 8 in parallel, ~5 min;
- `make lockdep-check`, which gains a `prd8` run.

What the stress can and cannot show with the BKL on: syscalls and reaping
are still serialized, so the use-after-frees this step removes cannot happen
yet. Today the stress is a regression load. It exercises the new locks for
real (the fault path, procfs readers against fork/munmap, exec), with
lockdep checking order, IRQ safety and held-across. It also found the procfs
bug above. Phase D1/D2 (syscalls and page faults without the BKL) is where
it becomes the actual race test.

## Verification

- `-Wall -Wextra`: 41 warnings, all pre-existing (same count as
  `feat/lock-core`), in both the normal and the `-DSECOS_LOCKDEP` build.
  Every commit builds in both modes (checked by building each staged tree).
- Proc-race stress on the lockdep image, `--secs 20 --runs 3`, at -smp 4 and
  -smp 8: 8/8 checks each. MemFree samples were [1004860, 1004896, 1004652]
  kB and [1005532, 1005520, 1005632] kB. No lockdep report, `[LOCK] BUG`,
  heap corruption, panic or `[EXC]`. Before the procfs fix the same runs
  failed only the reader content check (stale slots); there were no kernel
  faults.
- `make lockdep-check`: **18/18**. That is the 15 checks from A1 plus the
  new prd8 run (3 checks). Sub-results: ssd4/ssd8 17/17, off4 (`bkl=off`)
  12/12, nsd4 24/24, prd8 8/8, boot negative controls 10/10.
- The relevant selftest gates, run as a subset of `tools/selftest.sh` (same
  sections, unchanged) in a private `/tmp`: **331 passed, 0 failed**. They
  cover:
  - M7–M20: the M15 kill, M16 exec/wait, M17 blocking, M18 mmap/mprotect,
    M19 COW fork and M20 file mmap demos, plus execve (31 checks);
  - M25 pipes and M30 signals;
  - the bash command-substitution, exit-status/signal and job-control
    regressions;
  - KERNEL INTERFACES (procfs, 51);
  - USERS + PERM (40);
  - M48 SMP (45; x2APIC skipped as usual);
  - the new PROCRACE gate (18).

  The full suite was not run; that is left to the integrator.

## Plan notes (vs `docs/BKL_REMOVAL_PLAN.md`)

- The "24 call sites" are 18 `process_find_by_pid`, plus the 2
  pointer-returning wait-any/job-control helpers, 3 `process_get_last`, and
  one collect-then-post `process_foreach` (`signal.c` `pg_cb`) that kept
  pointers after the lock was dropped.
- "exec teardown under `mm_lock`": only the swap is under the lock. The old
  space is destroyed after the swap, because nothing can reach it by then.
- The plan mentions only `vmm_unmap_in_space`. `vmm_protect_in_space`,
  `vmm_fork_space`'s rewrite of the parent PTEs and `vmm_cow_fault` are
  covered too.
- Left for B2: `pagecache_get_phys()` returns a physical address with no
  pin. The fill in `vma_fault_in_live` copies from it outside every lock, so
  an eviction between the lookup and the copy would copy a recycled frame.
  B2's filling/publish protocol must hand out pinned pages, or the fault
  must copy under `pc_lock`.
- Left for C2: snap_cb still reads the target's `cwd`, fd table and
  `ino->path` without the owner's cooperation. With the BKL on this is
  benign; it is torn at worst, bounded and NUL-terminated.
