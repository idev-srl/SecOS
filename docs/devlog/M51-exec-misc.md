# M51 C6 — exec staging, sysroot/pkg locks, leftover global state

Phase C, step C6 of the BKL removal (`docs/BKL_REMOVAL_PLAN.md` §4, branch
`feat/exec-misc-locks`, from `42a04cb`). It also covers B5. The BKL stays on,
and the default boot behaves as before. Lock API: `docs/devlog/M50-lock-core.md`
("For phases B–D").

## B5: exec staging (`kernel/syscall.c`, `kernel/process.c`)

**Before.** `ksys_spawn_argv` read every image into one static 2 MB
`g_spawn_buf`. Then `process_create_from_elf_args` verified the signature on
that buffer and pinned a copy of it. Without the BKL, a second CPU spawning at
the same moment refills the buffer between the check and the copy. The pinned
image, which is what the FILE VMAs fault in from, could then be a different,
unverified file.

**The plan's fix** was `exec_lock` (K10) around the buffer. That would also
serialize the Ed25519 verify, which is most of an exec's 16–34 ms. So C6
removes the shared buffer instead:

- **`process_create_from_elf_owned(img, size, argc, argv)`** (new). It takes a
  kmalloc'd buffer and uses it as the pinned image (no second copy), or frees
  it on failure.
  - `SYS_SPAWN` reads the file straight into a buffer of exactly the file's
    size (`spawn_read_image`) and hands it over.
  - The size bound is `SPAWN_MAX_IMAGE` = 16 MB, the same as execve's
    `EXEC_MAX_IMAGE`. It used to be the 2 MB of the static buffer.
  - execve already used a private buffer per call (`process_exec` owns it), so
    it is unchanged.
- **The signature is now checked on the pinned copy.**
  - `proc_image_build` copies first (copy path), then verifies `im->image`, the
    bytes the process will actually run.
  - This closes the same time-of-check/time-of-use gap for every other caller
    that passes a shared buffer: the kernel shell's `g_job_spawn_buf` and the
    boot demos.
  - The only cost is a copy before a refusal.
- **Memory bound.**
  - A spawn never blocks between its kmalloc and the hand-over, and ring 0 is
    not preemptible. So each CPU has at most one staging image in flight.
  - That image *is* the pinned copy the old code made anyway.
  - Peak memory is lower than before (one copy per spawn instead of two), and
    the 2 MB of BSS is gone.
- **The loader is reentrant.**
  - The signature check (`mm/elf_sign.c`), SHA-256 and Ed25519 (`crypto/`)
    have no mutable static state; only `const` tables remain.
  - The other shared state on the load path already has its own lock: pid
    allocation and home-CPU choice (`proc_lock`), ASLR (csprng), kernel
    stacks (`kstack_lock`), the address space (PMM/heap) and driver bindings
    (`driver_if_lock`).
- **K10 `exec_lock` is not introduced.** `lock_order.h` keeps the level as
  "reserved, unused".

**Measured** (in-kernel test below, 4 vCPUs, TCG):

| Build | Serial | Parallel | Load throughput |
|---|---|---|---|
| Release | 12 loads in 214 ms | 48 loads in 238 ms | ×3.59 |
| `-DSECOS_HEAP_POISON` | 12 loads in 257 ms | 48 loads in 277 ms | ×3.71 |
| `-DSECOS_LOCKDEP` | 12 loads in ~460 ms | 48 loads in ~5.8 s | ×0.3 (see below) |

So execs on different CPUs no longer have to serialize once the BKL is gone.

## Revocation list (`mm/elf_sign.c`)

- The list is append-only, at most 16 entries.
- **Writers** (`elf_sign_revoke_key`, at boot or policy time) are serialized by
  `elf-revoke`, an S64 leaf spinlock. A writer stores the entry, then
  release-stores the count.
- **Readers** take no lock: every exec on any CPU acquire-loads the count.
- A revocation that races an exec which already passed the check does not stop
  that exec. This is the same as a revocation landing a moment later.
- `elf_sign_revoked_count()` was added for the test.

## `sysroot_lock`, `pkg_lock` (K11)

**`kernel/sysroot.c`.**
- The table `g_sf`, the hash cache, the manifest buffers `g_mf_*`/`g_mr`,
  `g_cmpbuf`, `g_akbuf`, the stats and the function-local static buffers are
  all scratch state of one provisioning run.
- `sysroot_provision`, `sysroot_apply_settings` and `sysroot_install_exec` are
  each one `sysroot_lock` section. The `_locked` bodies carry
  `KLOCK_ASSERT_HELD`.
- Inside the lock: VFS (K20/K30 and below), `kbd_lock` (S50) and the heap. No
  user copy, no spawn, no block.
- The entries never call one another.
- `g_persistent`/`g_rootdev` are written once by `sysroot_set_root` at boot,
  before any user process, and only read after that (procfs).

**`kernel/pkg.c`.**
- It has no static state. `pkg_lock` makes one install one section, so two
  installs never interleave their files.
- `pkg_lock` sits at the same level as `sysroot_lock` (K11); the two are never
  nested.
- The caller's buffer must not change during the install (it is verified, then
  unpacked). All callers pass a private kernel buffer.

**Bug fixed in `pkg.c`.**
- `sig_offset + 64 > len` could wrap for a `sig_offset` near 2^64. The header
  check then passed and SHA-256 read past the buffer.
- `data_offset + data_size` had the same problem.
- Both are now checked overflow-safe. Before the signature check, these fields
  are attacker-controlled.

## Other changes from the audit

- **`kernel/kiface.c`.** `g_ctxt` (every context switch, every CPU) and
  `g_forks` were plain `++` → atomic adds, otherwise concurrent updates are
  lost. The rest is documented in the file.
- **`kernel/klog.c`.** The destructive syslog cursor `g_rd` only moves forward
  now (CAS max). Two concurrent `SYSLOG_ACTION_READ` callers copy to user
  space outside the log lock. They may both return the same bytes, but one can
  no longer rewind the cursor over what the other consumed.
- **`kernel/power.c`.** The final phase (sync, unmount all, ACPI) is latched to
  one CPU. Two concurrent `reboot()` callers each walk their own per-PCB phases
  and would both run `vfs_sync_all`/`vfs_unmount_all` at once. The loser parks
  its CPU in `sti; hlt`, so it keeps answering TLB shootdowns.
- **`drivers/fbdev.c`.**
  - `g_kd_mode`/`g_kd_owner` and the fb console's graphics flag now change
    under `con_lock` (`drivers/vt.c`, the raw recursive console output lock).
  - The mode switch and the repaint of the text console are one section. A
    `KDSETMODE` racing the graphics owner's exit can no longer leave the mode
    and the screen disagreeing.
  - Other CPUs' console output can no longer land between "text again" and the
    repaint. `fb_console_redraw` used to run with no console lock at all.
  - `KDGETMODE` copies to user space with no lock held.
- **`mm/heap.c`.** `heap_usage(&used, &footprint)` was added for the test's
  leak check. The heap never gives frames back to the PMM, so "PMM free + heap
  footprint" is the quantity that must stay constant.

## Global mutable state audit

Scope: everything named in the C6 brief, plus the exec path. Files owned by
C1–C5 were reviewed but not edited.

| State | Where | Status |
|---|---|---|
| `g_spawn_buf` (2 MB exec staging) | kernel/syscall.c | **Removed**: per-spawn buffer that becomes the pinned image (B5). |
| execve staging | kernel/syscall.c | Already per call (kmalloc; `process_exec` owns it). |
| signature check source | kernel/process.c | **Fixed**: verifies the pinned copy, not the caller's buffer. |
| SHA-256 / SHA-512 / Ed25519 | crypto/ | Reentrant: only `const` tables. |
| revocation list | mm/elf_sign.c | **Locked**: S64 writer lock, lock-free acquire readers. |
| sysroot tables/buffers/stats | kernel/sysroot.c | **K11 `sysroot_lock`** around each exported entry. |
| `g_persistent`, `g_rootdev` | kernel/sysroot.c | Write-once at boot, before userland. |
| package install | kernel/pkg.c | **K11 `pkg_lock`**; no statics; overflow checks fixed. |
| per-CPU user/sys/idle ticks | kernel/kiface.c | Safe: each slot written by its own CPU's tick; aligned 64-bit reads. |
| `g_load` | kernel/kiface.c | Safe: written by CPU 0's tick only. |
| `g_ctxt`, `g_forks` | kernel/kiface.c | **Atomic adds** (were lost updates). |
| `g_last_pid` | kernel/kiface.c | Safe: single aligned store. |
| `p->ki.cmdline/comm` | kernel/kiface.c | Written by the owner's exec (or before publication). A /proc reader may see a mix of old and new text, never past the array. Accepted. |
| syslog `console_level` | kernel/kiface.c | Benign: int store, value unused. |
| klog ring, line, CLEAR mark | kernel/klog.c | `g_klock` (bounded irqsave), unchanged. |
| syslog read cursor `g_rd` | kernel/klog.c | **Monotonic CAS**; concurrent destructive readers may duplicate output, never rewind. |
| kmsg `rec[]`, debugcon line state | kernel/klog.c | Under `g_klock` / `g_dbglock`; per-CPU line buffers with IRQs off. |
| `g_real_base_ns`, `g_kclock_ready` | kernel/kclock.c | Written under `kclock` (S64), read as aligned 64-bit; double-checked init. Safe. |
| reboot phases | kernel/power.c | Per PCB. **Final phase latched** to one CPU. |
| NIC config, DHCP state | net/netctl.c, net/dhcp.c | Under the M47 stack lock; user copies on a kernel copy. Name/MAC/irq_mode written at NIC init only; `link_up` racy read is benign. |
| KD text/graphics mode | drivers/fbdev.c | **Now under `con_lock`** together with the repaint. |
| fb0 mmap | drivers/fbdev.c | Per process, under `mm_lock` (B1). |
| input devices / clients | drivers/input.c | `g_input_lock` (S15). Registration is append-only and never removed; `input_poll_devices` reads `g_ndev`/`g_dev[]` unlocked and skips a NULL slot. Safe. |
| csprng key/counter/reseed | crypto/csprng.c | `csprng` S64 irqsave. `csprng_stir_tsc` (timer IRQ) is lock-free and lossy by design (entropy mixing). See "For phase D". |
| capability audit ring | kernel/cap.c | Atomic head + per-record seq (B1). |
| driver bindings, rate limit, shadow regs, audit | kernel/driver_if.c | `driver_if_lock` (B1); `shadow_get` kmallocs under it (S70 inside S15, legal). |
| kernel shell globals (`g_cwd`, `g_jobs`, `g_job_spawn_buf`, pager) | kernel/shell.c (C5's file) | Single owner: only the BSP idle task runs the kernel shell, every command under the BKL (`execute_command`). Its shared spawn buffer is safe since the loader verifies its own copy. |
| boot demo load buffers (`m9_loadbuf`, …) | kernel/kernel.c | Boot only, before userland. |

## Lock order

- **K10.** Reserved, unused.
- **K11.** `sysroot_lock` and `pkg_lock`, never nested.
  - Both are taken under at most the BKL.
  - VFS, FS, block and spinlocks nest inside.
- **S64.** `elf-revoke`, a writer-only leaf.
- **S50.** `con_lock` now also covers the KD mode. Inside it: `cap_audit` →
  debugcon (the per-CPU line buffer and then `g_dbglock`, which is already
  nested inside `con_lock` by terminal output) and `fb_console_redraw`.
- `kernel/lock_order.h` says so.
- **No re-entrancy problem found.** None of the new locks is held across a user
  copy, a block, `switch_to` or `cpu_wait_irq`.

## Tests

### In-kernel forced concurrency (`kernel/exec_test.c`, `-DSECOS_EXEC_TEST`)

- **What runs.** The BSP runs from `kernel_main`; every AP joins from the idle
  hook in `kernel/smp.c`. All of them run the loader outside the BKL.
- **Workload.**
  - Images: hello (14 KB), coreutils (42 KB) and bash (1.1 MB), plus tampered
    copies (one flipped byte).
  - Both entry points: the copy path, and the owned-buffer SYS_SPAWN path.
  - Each process is torn down again (`process_detach_unstarted` +
    `process_destroy`).
- **Checks.**
  - Every signed image loads, and its pinned copy is byte-identical to the
    original.
  - The copy path never pins the caller's buffer; the owned path pins exactly
    the given one.
  - Every tampered image is refused.
  - Every worker also revokes the same 6 unused key ids. Each must be listed
    exactly once, and key 0 must stay trusted.
- **Leak check.**
  - One parallel warm-up round, then a serial round and two parallel rounds.
  - PMM free + heap footprint must stay unchanged, and heap bytes in use must
    stay unchanged.
  - `heap_check()` must pass.
- **Results.**
  - `-DSECOS_LOCKDEP`, `-smp 4`: **13/13 OK**, lockdep reports 0. The heap grew
    1140 KiB (one more bash image in flight than in the warm-up). The PMM + heap
    total and the bytes in use were identical.
  - Release and `-DSECOS_HEAP_POISON` builds: 12/12 and 13/13.
  - All CPUs loaded: cpu0 48, cpu1–3 36 each.

**Why the lockdep build is slower in parallel** (×0.3).
- `/proc/lockstat` from the run shows the cause: `heap` had 430 of 2144
  acquisitions contended, with a **max wait of 408 ms**.
- Every image buffer is kmalloc'd and kfree'd under the global
  test-and-set `heap_lock`. That lock is unfair, and under lockdep's
  instrumentation and TCG it starves waiters.
- The same workload in a release or poison-only build scales ×3.6, so this is
  not a loader lock.

### Userland exec stress (`tools/exec_stress_drive.py`, new selftest "M51 EXEC GATE")

- **Setup.** One `-DSECOS_EXEC_TEST -DSECOS_LOCKDEP` CONSOLE=root image at
  `-smp 4`.
- **Workload.**
  - `bash -c true` ×500 in 4 parallel loops.
  - A fifth loop runs a tampered copy of `/bin/coreutils` (one flipped byte),
    which must fail with 126, i.e. execve refused.
- **Checks.**
  - MemFree steady across two measured runs (slack 1 MiB).
  - Processes ran on ≥ 2 CPUs.
  - No fault, LOCKDEP or `[LOCK] BUG` marker in the log.
- With the BKL on, these execs still serialize on it. The gate checks
  correctness and the absence of leaks, and records the exec rate.
- Results: see "Verification".

## For phase D

- **exec/spawn hold no exec-wide lock.**
  - Clearing `SYS_EXECVE`/`SYS_SPAWN` from `syscall_bkl_mask` needs the VFS
    and FS locks of C1–C4: `vfs_lookup` + `ino->ops->read`, where the inode
    pointer is held without a reference, like every VFS caller today.
  - It also needs the pty/cred/cap state those calls touch, all locked since
    B1/C5.
  - `process_create_*` / `process_exec` themselves are ready.
- **`heap_lock` is the next exec bottleneck.** It is a global, unfair
  test-and-set spinlock with a first-fit walk, and the lockdep build poisons
  under it. A ticket lock or per-size caches would help parallel exec most.
- **`csprng_get` holds its irqsave lock for the whole request.**
  `/dev/random` (devfs, C2) passes the user's length, so a large read keeps
  IRQs off for its whole duration. Chunk the request (e.g. 4 KB per lock hold)
  before devfs leaves the BKL.
- **`kernel/shell.c` needs a lock if the kernel shell is ever off the BKL.**
  Its globals assume one task under the BKL; keep it there or give it its own
  klock.
- **Concurrent destructive syslog readers can return duplicate data.** Real
  serialization would need a klock not held across the user copy (copy into a
  bounce buffer under it, then copy out).

## Plan corrections

- **The plan asked for `exec_lock` (K10) around `g_spawn_buf`.** Removing the
  buffer is strictly better: no lock, parallel verifies, less memory. K10 is
  left unused.
- **The real bug was not only the shared buffer.** The signature was checked on
  the caller's buffer and a *copy* was pinned, so any shared source was a
  time-of-check/time-of-use gap. Fixed in the loader for every caller.
- **The plan grouped the KD/console mode with nothing.** It needed the console
  lock, which was already the right lock: no new lock was added.

## Verification

- **Warnings.** `-Wall -Wextra`: 41 warnings (baseline) in the
  `-DSECOS_LOCKDEP -DSECOS_EXEC_TEST` build; none in the touched files (the
  only `process.c` warning is the pre-existing fork one).
- **Boot self-test.** `[EXECTEST]` 13/13 on `-smp 4` (lockdep), lockdep silent.
- **Exec stress** (`tools/exec_stress_drive.py`, lockdep image, `-smp 4`,
  count 500): 9/9.
  - 2 × 500 `bash -c true` with 0 failures, and 0 tampered execs accepted.
  - MemFree went 1002112 → 1002188 kB.
  - Processes ran on CPUs 1–3; no fault markers.
  - The combined gate image (`-DSECOS_EXEC_TEST -DSECOS_LOCKDEP`) passed 9/9 at
    count 40, plus `[EXECTEST] DONE ok=13 fail=0`.
- **Gates** (extracted from `tools/selftest.sh`, private /tmp):
  - M9 5/5, M16 4/4, execve 32/32, Phase K (M31 + M32 pkg) 8/8, M46 27/27,
    M41 PERSIST 30/30, USERS 15/15.
- **`make lockdep-check`:** 17/18.
  - ssd4, ssd8, off4, nsd4 and prd8 are all lockdep-clean, and all 10 negative
    controls were caught.
  - The one failure is the prd8 `shell-race` step, which timed out at 600 s
    (the command did finish later).
  - It timed out again in a solo rerun.
  - A baseline 42a04cb lockdep image passed it the same afternoon. Comparing
    the two showed the cause is not this branch: the lockdep image's exec rate
    is **bimodal per boot**.
    - In one A/B run, the 42a04cb image did 5.6 execs/s.
    - In another, the same baseline image did 1.1/s while this branch's image
      did 5/s.
    - TSC/LAPIC calibration and CPU spread are identical in fast and slow
      boots.
  - A slow boot runs `shell-race` (thousands of busybox execs) past its budget.
  - Worth a look in phase D: a likely suspect is the heap layout after boot,
    given the 408 ms `heap_lock` waits seen in the lockdep build.
  - The baseline image also drifted 1.3 MB in MemFree in one 100-exec run
    (noise or a pre-existing slow leak; not reproduced on this branch).
