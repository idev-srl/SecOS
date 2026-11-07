# BKL removal plan for SecOS (M50–M52)

Written from main d062882 by a planning agent. Line numbers refer to that commit. The design is fixed, but implementers must check every claim against the code.

The two things that shape this plan:
- **No sleeping mutex is needed.** Ring 0 is not preemptible. A blocked syscall rewinds and runs again from the start. All disk I/O is polled. So no critical section ever has to block. The BKL is already a lock that belongs to a task and spins with interrupts on. That primitive only needs to become generic.
- **The BKL hides five bugs (B1–B5).** They must be fixed before any subsystem can drop the BKL.

## 1. How the BKL works today

### Where it is taken
| Path | Location |
|---|---|
| Every syscall body | `kernel/syscall_trap.c:71-75` (`bkl_lock_enable_irq` … `bkl_unlock`) |
| User-address page fault | `kernel/panic.c:255-262`. From ring 3 it spins with IRQs on; from ring 0 it is taken recursively. |
| Reap | `kernel/sched.c:336` (`process_destroy`) |
| exit / kill / stop | `sched.c:371`, `:403`, `:429` (`switch_to` drops it) |
| Kernel shell | `kernel/shell.c:2088` (`execute_command`) |

### Where it is dropped or suspended
- `switch_to` → `bkl_drop_for(prev)` (`sched.c:175`). It is dropped on block, exit, kill, stop and yield, like the Linux BKL.
- The block re-run path drops it before `arch_iret_to_tf` (`sched.c:459`).
- The kernel shell wraps its waits in `bkl_suspend`/`bkl_resume`: the pager at `shell.c:193`, `sh_wait_tick` at `shell.c:1660`.

### Rules
- Recursive for its owner. The owner is the current task, or the `cpu_t` before the scheduler starts (`bkl.c:14-31`).
- Never taken by an IRQ handler.
- Outermost of all locks.
- The tick (`sched.c:240`) and the reschedule IPI (`sched.c:278`) never preempt an idle task that holds it.

### IF state
- INT 0x80 is a trap gate (`idt.c:157`, `0xEE`), so syscalls run with IF on. The kernel is still not preemptible: the tick only preempts ring-3 code (`sched.c:249`).
- **BUG:** contended `bkl_acquire` does `sti`, spins, then `cli`, and returns with IF=0 (`bkl.c:27-29`). The new primitive must save and restore the caller's IF.

### Primitives available
- `arch/x86/spinlock.h`: lock, trylock and irqsave variants. Spinning calls `cpu_relax_poll()`→`tlb_poll()`, which keeps TLB shootdowns safe.
- `net/net_lock.h`: a level-ordered lock with `-DSECOS_LOCKDEP_LITE` (net only).
- No mutex and no wait queue. Blocking is `sched_block_current` + rip-rewind + `g_wake_seq` (`sched.c:447-465`). Every caller is in `syscall_trap.c` or `kiface.c:183`, after the subsystem has already returned.

## 2. Inventory

### Already safe on their own locks
`pmm_lock`, `heap_lock`, `kstack_lock`, `g_pm_lock`, `tlb_lock`, `g_pipe_lock`, `g_ipc_lock`, `g_pty_lock`, `g_input_lock`, `kbd_lock`, `proc_lock`, kclock, csprng, klog/debugcon, `con_lock` (vt.c:679), the whole net stack, `g_sysfs_lock`, procfs `g_proc_lock` (procfs.c:467).

### Relying on the BKL
| Subsystem | Problem without the BKL |
|---|---|
| ext2/ext4 + JBD2 | Global state in `fs/ext2.c:47-136`: scratch buffers `g_b/g_mb/g_xb/g_eb/g_tb/g_csb`, `g_jt[]` (:1644), the vslot pool (:846), bitmaps. The FS gets corrupted. |
| fat32 | Global `g_sec`, FSInfo, the pool (`fat32.c:33-79`, `:331`). |
| exfat | `g_bounce`, `g_cbuf`, bitmap, pool (`exfat.c:117-139`, `:937`). |
| ntfs | `g_vol[]`, shared `g_bounce` (`ntfs.c:207-209`). |
| ramfs | `ramfs_table` and `inode_cache` (`ramfs.c:13`, `ramfs_vfs.c:14`). This is the heap corruption behind the sshd.log crash seen in M48. |
| devfs | Lazy `g_built` (:93). Raw `/dev/<blk>` I/O calls `b->read/write` directly and **bypasses `block_io_lock`** (devfs.c:282-288). |
| VFS | `g_mount_lock` covers only the mount table. mount/umount/remount are not serialized against running ops. **O_APPEND looks up the size, then writes** (`syscall.c:731-736`). |
| page cache | `pc_lock` is held across `ino->ops->read` (pagecache.c:27-57). That deadlocks once FS locks exist (B2). |
| block | One global irqsave `block_io_lock` (block.c:15). Partitions call `parent->read` directly (partition.c:30-42). |
| xHCI/USB | Global state. `usb_hid_poll` is called from syscall context on any CPU (keyboard.c:160/180) while `usb_msc` uses the same event ring. |
| console TTY | `g_con` has no lock (tty.c:42-45, :84). `pull_input`/`wait_input` do `sti; hlt` (tty.c:394, :463). `g_foreground_pgid` (signal.c:27). |
| process lifetime | `process_find_by_pid` returns a pointer without taking a reference (process.c:254-262, 24 call sites). The reaper `kfree`s the process → use-after-free. |
| other address spaces | procfs `snap_cb` walks the target's VMAs and page tables (`vmm_count_user_pages`, procfs.c:150) while the target may munmap and free them. |
| exec/spawn | Static 2 MB `g_spawn_buf` (syscall.c:87, :138). |
| misc | `driver_if.c:12-14`, the `cap.c:11-12` audit ring head, `sysroot.c` statics, `elf_sign.c:51` revocation list, cross-process credential reads. |
| kernel shell | `ls_cb` shows the pager prompt inside a readdir callback (shell.c:1232-1247). |

## 3. Lock design

### 3.1 New primitive: `klock_t` (`kernel/klock.{c,h}`)
The BKL machinery made generic and non-recursive:

```c
typedef struct klock { void* volatile owner; uint16_t level; uint8_t recursive;
                       uint32_t depth; const char* name; uint64_t t_acq; } klock_t;
void klock_lock(klock_t*); void klock_unlock(klock_t*); int klock_trylock(klock_t*);
int klock_held(const klock_t*);
```

**Behaviour:**
- The owner is the current task, or the `cpu_t` before the scheduler starts.
- It spins with IF on whenever the caller had IF on, using pause + `tlb_poll`.
- It returns with the caller's RFLAGS.IF exactly as it was.
- It counts `cur->klock_depth`. The tick and the IPI check `klock_depth > 0` instead of `bkl_held_by` (sched.c:240, :278).

**Forbidden** (lockdep panics; release builds log `[LOCK] BUG`):
- taking a klock in IRQ context (needs a per-CPU `irq_depth`);
- taking a klock while holding a spinlock;
- holding a klock across `switch_to`, `sched_block_current` or the return to ring 3;
- holding a klock during `copy_to/from_user`, because a fault on a file-backed mmap re-enters the FS.

**Exception:** only the BKL itself (the one recursive klock) is dropped by `switch_to` when `bkl=on`.

**Why not a sleeping mutex:** sleeping would need ring-0 continuations, and that breaks the M48 migration invariant (a READY task's state is trapframe + FPU). Polled I/O bounds every hold time, so spinning is acceptable.

### 3.2 Lock order (`kernel/lock_order.h`, outer → inner)
All K locks (klocks) come before all S locks (spinlocks). A task never holds two FS-instance (K30) locks at once: the VFS drops one before entering another mount.

| Level | Lock | Type | Protects |
|---|---|---|---|
| K0 | `bkl` | recursive klock | fallback only |
| K10 | `exec_lock` | klock | `g_spawn_buf` / exec staging |
| K11 | `sysroot_lock`, `pkg_lock` | klock | |
| K20 | `vfs_ns_lock` | klock | serializes mount/umount/remount/replace_root |
| K30 | one FS-instance lock per driver (all drivers are singletons; ntfs gets one lock because of the shared `g_bounce`) | klock | all driver state, inode pool, JBD2 transaction. Taken at every exported op, including mount/umount/sync/remount. `mntinfo` stays lock-free. Instances: ramfs, ext2, fat32, exfat, ntfs, devfs. |
| K40 | `blkdev->lock` on the BASE disk (a partition resolves to its parent; AHCI: one per port) | klock | |
| S10 | `tty_con_lock` (new), `g_pty_lock` | irqsave | never held across hlt |
| S15 | procfs `g_proc_lock`, `g_sysfs_lock`, `g_input_lock`, `g_pipe_lock`, `g_ipc_lock`, `driver_if_lock` (new) | irqsave | |
| N0–N3 | net tick → rx → stack → tx (M47) | irqsave | |
| S20 | `g_mount_lock` | irqsave | mount table only |
| S30 | `proc_lock` | irqsave | process table, refcounts, credential changes |
| S35 | `p->mm_lock` (new, per process) | irqsave | VMA array + page-table edits/frees + cross-process readers |
| S40 | `pc_lock` | irqsave | page-cache index only; no FS call under it |
| S45 | `xhci_lock` (new) | irqsave | HID poll uses trylock |
| S50 | `kbd_lock`, `con_lock`, klog/debugcon | | |
| S60 | `kstack_lock`, `g_pm_lock`, `kvmm_lock` (new, kernel-half page-table edits), `tlb_lock`, kclock, csprng | | |
| S70 | `heap_lock` → `pmm_lock` | | |

**Typical paths:**
- cached read: `pc_lock` only;
- cache miss: K30 → K40 → S45;
- write: K30 → K40, with invalidate (S40) inside K30, legal once B2 is done;
- procfs: S15 → S30 → S35.

### 3.3 Lockdep (`-DSECOS_LOCKDEP`, which absorbs `SECOS_LOCKDEP_LITE`)
- **Lock classes:** `spinlock_t` gets `{level, name}` through `SPINLOCK_INIT_CLASS(lvl, "name")`. An unclassified lock is checked only for being held across hlt or `switch_to`.
- **Tracking:** a per-CPU stack of held spinlock classes and a per-task stack of held klocks.
- **Checks:**
  - lock order and recursion;
  - a klock taken under a spinlock;
  - any lock still held at syscall exit, at iret to ring 3, at `switch_to`, or across `cpu_wait_irq()`. This is a new wrapper that replaces every raw `sti; hlt` in kernel paths.
- **`ASSERT_HELD`** in internal helpers.
- **Watchdog:** a wait longer than 2 s (TSC) prints `[LOCKDEP] stuck on <name> owner=pid/cpu since=<tick> mine: <held list>`. The same dump is printed from panic.
- **Heap poisoning:** `kfree` fills freed memory with 0x6B.
- **`/proc/lockstat`:** acquire, contended and max-wait counts per lock class.

## 4. Staging
The default stays `bkl=on` until D7, so every step is tested on its own.

### Phase A — foundation (one agent, sequential), devlog M50

**A1 `feat/lock-core`**
- `kernel/klock.{c,h}`, `lockdep.{c,h}` and `lock_order.h`.
- `bkl.c` reimplemented on top of klock with the same behaviour, including `bkl_drop_for`.
- In `sched.c`, `klock_depth` replaces `bkl_held_by`.
- `cpu_wait_irq()` replaces the raw `sti; hlt` in `tty.c`, `shell.c` and `net/*`.
- The net lockdep moves into the generic one.
- Every existing spinlock gets a class.
- **Tests:** full selftest, plus a LOCKDEP build through `smp_stress_drive` (-smp 4 and 8) and NETSMP. Lockdep must stay silent with the BKL on.

**A2 `feat/bkl-switch`** (may be folded into A1)
- `g_bkl_mode`, default on.
- `bkl=on|off` on the MB2 command line (`kernel.c:2423`).
- UEFI load options passed through `secos_boot_info`; today `uefi/boot.c:50` only parses `nokaslr`.
- A GRUB entry "SecOS x64 (kernel lock: bkl=on)".
- `syscall_bkl_mask[]` lists the syscalls that still take the BKL; `bkl=on` forces all of them.
- `/proc/smp` reports the mode.

### Phase B — prerequisite fixes (BKL still on; B1, B2 and B3 run in parallel)

**B1 `feat/proc-lifetime`**
- A `p->refs` count, `process_get_by_pid()`/`process_put()` (the reference is taken under `proc_lock`), and `process_reap_one` (process.c:355) skips a zombie whose refs are above 0.
- All 24 `process_find_by_pid` call sites converted.
- A per-process `mm_lock` covering: VMA edits, `vmm_unmap_in_space`, munmap/brk/exec teardown, and the PTE install in the fault path (taken after the page-cache page has been obtained without the lock), plus procfs `snap_cb`.
- Credential changes under `proc_lock`.
- A new `driver_if_lock`; the cap audit head becomes an atomic `fetch_add`.
- **Files:** `process.{c,h}`, `sched.c`, `signal.c`, `tty.c`, `pty.c`, `syscall*.c`, `kiface.c`, `shell.c`, `vma.c`, `vmm.c`, `procfs.c`, `driver_if.c`, `cap.c`.
- **Test:** a proc-race stress (8 loops of `kill -0` + `/proc/*/status` + `/proc/*/maps` against fork/exit/mmap bursts) on a heap-poisoned LOCKDEP build.

**B2 `feat/pagecache-fill`**
- `pc_lock` is never held across `ops->read`: reserve a "filling" slot under the lock, unlock, fill, re-lock, publish.
- Concurrent missers spin with IF on while the slot is filling.
- `pagecache_invalidate` bumps a per-inode invalidation generation, and publication is refused if the generation changed during the fill.
- **Test:** M20, mmap/write coherence, fsstress mmap mode.

**B3 `feat/block-locks`**
- A `block_dev_t.lock` (K40) on the base disk; `block_read`/`block_write` resolve a partition to its parent disk.
- devfs raw I/O goes through `block_read`/`block_write`.
- `xhci_lock` around every transfer and around enumeration; `usb_hid_poll` uses trylock.
- The global `block_io_lock` is deleted.
- **Files:** `fs/block.{c,h}`, `partition.c`, `devfs.c`, `xhci.c`, `usb_msc.c`, `usb_hid.c`, `keyboard.c`.
- **Test:** M10×3, M21, M22, parallel `dd` on `/dev/vda` and `/dev/vdb` while a usb-kbd is in use.

**B4** The kernel shell `ls_cb`/`vls_cb` collect entries first, then page (shell.c:906, :1232). Can be folded into C5.

**B5** `exec_lock` around `g_spawn_buf` (syscall.c:87-140). Can be folded into C6.

### Phase C — subsystem locks (BKL still on; parallel after B2 and B3, some also need B1), devlog M51

**Pattern for each FS:**
- One static `klock_t` per driver instance.
- Every `vfs_fs_ops` entry becomes `lock(); r = x_op_locked(); unlock();`.
- Internal callers use the `_locked` versions; lockdep catches recursive re-entry.
- The lock lives inside the driver because `syscall.c:662/743` and `sysroot` call `ino->ops` directly.

| Step | Branch | Scope |
|---|---|---|
| C1 | `feat/vfs-locks` | `vfs_ns_lock`. Unmount waits on the FS lock. **Atomic O_APPEND**: `write(ino, VFS_OFF_APPEND, …)` resolved under the FS lock and returning the offset (drop the lookup at syscall.c:731-736). Check that `vfs_walk` never holds an FS lock across mounts. Files: `fs/vfs.{c,h}`, `syscall.c` (append only), `kiface.c`. |
| C2 | `feat/fs-virt-locks` (after B1) | ramfs + ramfs_vfs, devfs (built at init, then locked), procfs/sysfs review. |
| C3 | `feat/ext4-lock` | ext2/ext4 + JBD2 + ext2ramdev: every op, plus `jbd2_recover`, sync and remount. |
| C4 | `feat/fat-exfat-ntfs-lock` | fat32, exfat, ntfs. |
| C5 | `feat/tty-locks` (after B1) | `tty_con_lock` over `g_con`. The 10 ms ESC wait in `pull_input` moves outside the lock (re-poll after `cpu_wait_irq`). `wait_input` drops the lock while spinning. `g_foreground_pgid` goes under the lock. Includes B4. Files: `tty.c`, `signal.c`, `shell.c`, `keyboard.c`. |
| C6 | `feat/exec-misc-locks` | B5, `sysroot_lock`, `pkg_lock`, the `elf_sign` revocation list, kiface leftovers. |

### Phase D — push the BKL down (one integrator, `integ/bkl-off`, sequential), devlog M52
Each step clears bits in `syscall_bkl_mask` or removes one call site. Every gate runs in both modes.

| Step | Removes the BKL from | Needs |
|---|---|---|
| D1 | getpid/time/sleep/yield, signals, sockets, pipe/IPC, `sched_*`, mmap/munmap/mprotect/brk | B1 |
| D2 | the page-fault path (`panic.c:255-262`) | B1, B2 |
| D3 | file syscalls: open/read/write/stat/getdents/rename/unlink/truncate/fsync | C1–C4 |
| D4 | TTY/pty/ioctl/poll. The `read`/`write` bits are cleared only here, because they dispatch on fd type. | C5 |
| D5 | fork/spawn/execve/mount/umount | C1, C6 |
| D6 | teardown (`sched.c:336/371/403/429`) and the kernel shell (`shell.c:2088`). The mask is now empty and `bkl_suspend` is no longer needed. | |
| D7 | — flips the default to `bkl=off` and keeps the code | |
| D8 | — later, after real-hardware validation: optionally delete the BKL | |

## 5. Parallelism
```
A ─┬─ B1 ─┐
   ├─ B2  ├─ C1 C2(B1) C3 C4 C5(B1) C6 ── D1..D7
   └─ B3 ─┘
```

**Overlaps and merge order:**
- C1 and C6 both touch `syscall.c` (append vs spawn region): merge C1 first.
- B1 and C2 both touch `procfs.c`: C2 starts after B1.
- B1 and C5 both touch `tty.c`: C5 starts after B1.
- B3 and C2 both touch `devfs.c`: merge B3 first.

## 6. Fallback
Keep `bkl=on`. It is the outermost K0 lock: it can only add serialization, never remove correctness. The default becomes off after D7. Delete it only after the real ASUS laptop passes GUI, SSH and `stress 32` with `bkl=off`.

## 7. Tests

**Existing gates:**
- the full selftest at every step;
- `smp_stress_drive` with -smp 4, 8 and 16 on a LOCKDEP build;
- NETSMP;
- `persist_test` with -smp 4 in both modes;
- SSH;
- `soak_drive` + fuzzer for 30 minutes at -smp 8 before D7.

**New tests:**
1. **`/bin/fsstress` + `tools/fs_stress_drive.py`.** N forked workers spread round-robin over the CPUs: create, write a pattern, append to a shared log, rename, unlink, mkdir/rmdir, truncate, getdents, stat, and mmap-read compared against read. At the end, verify in the guest: patterns, log line count, no torn lines up to 4 KB. Targets:
   - ext4 root at -smp 4 and 8, then host `e2fsck -fn`;
   - FAT32 `vdb`, then `fsck.vfat -n`;
   - exFAT, then `fsck.exfat -n` + deepcheck;
   - ramfs `/tmp`;
   - NTFS with 8 readers, checksums compared against the host;
   - ext4 and FAT32 at the same time.
2. **Crash consistency:** kill QEMU in the middle of fsstress on ext4, reboot so the journal replays, then host `e2fsck -fn` must be clean. 10 iterations.
3. **TTY/pty/pipe stress** (`smptest pty|pipe`): 16 pty echo pairs, 6 SSH sessions, and pipelines whose byte counts are verified.
4. **proc-race** (see B1).
5. **Mount race:** an exFAT/USB mount/umount loop while files are open. Expect EBUSY or a clean failure, never `[EXC]`.
6. **Exec stress:** `bash -c true` 500 times in parallel on 8 CPUs.
7. **Scaling report:** ops/s with `bkl=on` vs `bkl=off` at -smp 4, plus `/proc/lockstat`.

**Pass criteria:** no `[LOCKDEP]`, `[LOCK] BUG`, `[HEAP]`, `[EXC]` or panic; MemFree steady; host fsck clean.

## 8. Risks
| Risk | Mitigation |
|---|---|
| An FS re-entering itself (symlinks, rename, sysroot) | `_locked` helpers; lockdep panics on recursion |
| Callbacks run under a lock (readdir pager, procfs `process_foreach`) | Collect first, then act |
| Idle task preempted while holding a klock | `klock_depth` guard |
| Waiters with IF=0 on the BSP stopping the tick (the tick runs only on the BSP) | klock spins with IF on; spinlocks only for short sections; HID uses trylock |
| Stale page cache | B2 generation counter |
| Use-after-free on processes | B1 refcounts + heap poisoning |
| mount/umount racing open fds | C1 serializes and returns EBUSY |
| Long ext4 commits on USB making others spin | Accepted; watch `/proc/lockstat` |
| A hidden BKL dependency | `bkl=on` vs `bkl=off` A/B runs; the per-syscall mask can put one syscall back under the BKL |
