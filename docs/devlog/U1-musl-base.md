# U1 — musl bring-up (`feat/musl-base`)

Userland plan §3 U1, design §2 D1 (option B1), D2, D3, D4. SecOS programs can
now be written against **unmodified musl 1.2.6**: the Linux system calls musl
makes are translated to the SecOS ABI by a layer inside libc, and the kernel
got the few things every musl program needs (an auxiliary vector, a thread
pointer, a loader for standard ELF layouts). Existing programs (the home-made
`user/libc.c`, BusyBox, bash, Dropbear, lua, LVGL) are untouched and keep
running; they move to musl in U7.

## Quick start

```
tools/port-musl.sh                     # fetch (pinned SHA-256), build build/sysroot/ (~1 min)
tools/secos-cc -O2 -o hello hello.c    # compile + static-PIE link + sign (dev key)
tools/secos-cc -O2 -o tool tool.c -lsecos          # + SecOS-only calls, <secos/secos.h>
tools/secos-cc --secos-note=user/note_confined.S -o t t.c   # another manifest
tools/secos-cc --secos-unsigned -o x x.c           # not signed: SecOS refuses it
SECOS_SYSCALL_TRACE=1 ./prog           # on SecOS: report every call the layer can't serve
tools/musl_base_test.sh [--lockdep] [--smp N] [--iso FILE] [--keep]   # the MUSL-BASE gate
```

Programs reach a running SecOS on a disk for now (the gate uses an ext2 test
disk at `/mnt`); packages (U6) are the delivery route for real software.

## Kernel

### Auxiliary vector (`kernel/process.c`)

Every image (kernel-made, `SYS_SPAWN`, `execve`) now starts with the System V
/ Linux x86-64 stack: `rsp` points at `argc`, then `argv[]`, `NULL`, `envp[]`,
`NULL`, the auxv, and above them the strings, `"x86_64"` and 16 bytes for
`AT_RANDOM`. The auxv entries: `AT_PHDR/PHENT/PHNUM` (where the program
headers are in memory: `PT_PHDR`, else the `PT_LOAD` that contains them),
`AT_PAGESZ`, `AT_BASE` (0: no interpreter), `AT_FLAGS`, `AT_ENTRY`,
`AT_UID/EUID/GID/EGID`, `AT_SECURE`, `AT_PLATFORM`, `AT_HWCAP` (CPUID.1:EDX)
and `AT_HWCAP2`, `AT_CLKTCK` (100), `AT_RANDOM` (CSPRNG), `AT_EXECFN` (the
`execve` path, else `argv[0]`), `AT_MINSIGSTKSZ` (the SecOS signal frame +
red zone).

The stack is now built also for argc 0 (it used to be skipped, leaving `rsp`
at the very top of the stack VMA, where musl's `_start` would fault reading
`argc`). Old programs are unaffected: their `crt0` still gets argc/argv/envp
in `rdi/rsi/rdx`.

The credentials of a new image are settled after its stack is built (spawn
copies the spawner's ids, a set-id exec changes them). `process_auxv_set_creds()`
patches the five id slots (`AT_UID`..`AT_SECURE`, written in a fixed order) in
both paths, before the image runs. `AT_SECURE` = effective != real id.

### Thread pointer: `arch_prctl` (`kernel/syscall_lx.c`)

New Linux-shaped calls use **512 + the Linux number** (D3): the number says
which Linux call they mirror, and the musl table passes them straight on.
`kernel/syscall_trap.c` dispatches 512..1023 to `syscall_lx_dispatch()` with
six arguments.

| Number | Call | |
|---|---|---|
| 670 = 512+158 | `arch_prctl(ARCH_SET_FS / ARCH_GET_FS, addr)` | `-EPERM` for an address outside the user half (the value is then canonical, so `wrmsr` cannot fault); `ARCH_SET_GS/GET_GS` and others `-EINVAL` |
| 730 = 512+218 | `set_tid_address(int*)` | stores the pointer (`process_t.clear_child_tid`, used by threads in U9), returns the tid = pid |
| 743 = 512+231 | `exit_group(status)` | the `SYS_EXIT` path while a process has one thread |

`process_t.fs_base` is loaded into `MSR_FS_BASE` by `switch_to()` for every
user task (and by the two demo entry paths in `arch/x86/context_switch.c`).
The kernel never uses `%fs` (the stack-protector guard is a global since M44),
so the MSR only has to hold the running task's value; a task changes its own
value only through `arch_prctl` and exec, which load the MSR at once. fork
copies it with the PCB; exec clears it to 0. `CR4.FSGSBASE` stays off (user
space cannot write the base itself). The three calls touch only the caller's
own `process_t`: no lock, and `kernel/bkl.c` lists them as BKL-free
(`lx_lockfree()`); every other number >= 256 still takes the BKL until it is
listed.

### Loader (`mm/elf.c`)

`elf_load_image_lazy()` now:
- accepts **any `PT_LOAD` layout inside the user image window
  [`USER_CODE_BASE`, `USER_HEAP_BASE`) = [4 GiB, 10 GiB)**: code no longer has
  to be in [4, 8) GiB and data in [8, 16) GiB (the old `user.ld` layout is one
  valid case); each segment's end is checked too (only the start was);
- loads an **`ET_DYN` without `PT_INTERP` (static-PIE) at a random
  page-aligned base** in that window (CSPRNG, about 20 bits for a small
  program; `-DSECOS_NO_ASLR` loads it at 4 GiB). The gate sees a different
  base on every run;
- refuses `PT_INTERP` (no dynamic linking, D7), an executable `PT_GNU_STACK`
  (W^X), segments that overlap or share a page (one protection per page),
  `e_type` other than `ET_EXEC`/`ET_DYN`, more than 64 program headers, and
  program headers outside the file;
- maps read-only segments (`R`, no `W`/`X`: `.rodata`, the headers) read-only;
  they used to be RW (only the old two-segment layout existed);
- returns the layout facts (`elf_load_info_t`: bias, `AT_PHDR`, link and
  runtime entry, `PT_GNU_STACK` size, footprint).
The stack VMA is the `PT_GNU_STACK` size when the program asks for more than
the 32 KiB default (capped at 64 MiB): `secos-cc` links `-z stack-size=8M`, like
Linux's default limit. The manifest `entry_hint` is compared with the link-time
entry.

### Signal handlers

A handler is entered with `rsi = rdx = 0`: `SA_SIGINFO` handlers see NULL
`siginfo_t`/`ucontext_t` pointers instead of whatever the interrupted code had
in those registers. `siginfo_t` itself is U2.

## User space

### The port (`tools/port-musl.sh`)

1. fetches `musl-1.2.6.tar.gz` and the two upstream patches published after
   1.2.6 (the musl.libc.org advisories: CVE-2026-6042 iconv, CVE-2026-107891
   regcomp), all SHA-256 pinned, into `build/dl/`;
2. unpacks the **pristine** tree into `third_party/musl/` (git-ignored: like
   bash and BusyBox, musl is fetched, not committed; MIT,
   `docs/licenses/musl-1.2.6-COPYRIGHT`);
3. copies it to `build/musl-src/`, applies the upstream patches, then the
   **overlay** `user/musl-secos/` (files under `arch/` and `src/` replace or
   add musl files, `patches/*.patch` are applied, the script's `DELETE` list
   removes files whose generic version must win);
4. configures (static only, `-fPIE`, no wrapper) and installs into
   `build/sysroot/` (`include/`, `lib/libc.a`, `crt1.o`/`rcrt1.o`/`crti.o`/`crtn.o`);
5. **fails if `libc.a` still contains a `syscall` or `sysenter` instruction**;
6. builds `lib/libsecos.a` + `include/secos/secos.h` and `lib/secos-note.o`.

### The overlay (`user/musl-secos/`)

| File | Replaces | What |
|---|---|---|
| `arch/x86_64/syscall_arch.h` | musl's | `__syscall0..6` call `__secos_syscall`; no vDSO |
| `src/secos/syscall.c` | new | the translation table (below) |
| `src/thread/x86_64/__set_thread_area.s` | musl's | `arch_prctl(ARCH_SET_FS)` = SecOS 670 directly (runs before TLS) |
| `src/signal/x86_64/restore.s` | musl's | `__restore_rt`: `SYS_SIGRETURN` (45) |
| `src/thread/x86_64/syscall_cp.s` | musl's | cancellable syscall: checks the flag, calls the layer |
| `src/thread/x86_64/clone.s`, `__unmapself.s` | musl's | `-ENOSYS` / exit (threads are U9) |
| (deleted) `src/process/x86_64/vfork.s` | | the generic `vfork` = `fork` |
| `patches/posix_spawn-fork.patch` | | `posix_spawn` forks instead of `clone(CLONE_VM\|CLONE_VFORK)` |
| `libsecos/secos.c`, `include/secos/secos.h` | new | `secos_spawn/wait/getticks/sleep_ticks/msg_send/msg_recv/netctl/get_caps/reboot/sigverify` and raw `secos_syscall` |

`src/secos/` is built without the stack protector (a line appended to musl's
generated `config.mak`): the layer runs before TLS exists, and the canary is
read from `%fs:0x28`.

### The translation table (`src/secos/syscall.c`)

161 Linux numbers have an entry (about 125 served, the rest an explicit `-ENOSYS` with a reason). The layer never touches errno, calls no libc
function and returns what Linux returns (`-errno`); a SecOS call that still
answers a bare `-1` gets the most likely errno for that call (checked against
the kernel paths: `EBADF` for fd calls, `ENOENT`/`ENOTDIR`/`EACCES` from a
follow-up `stat` for path calls, `EPIPE` for a write to a pipe with no reader).

- **Direct** (renumbered): `read/write` (0 length answered here, count capped
  to the kernel's int), `close`, `lseek`, `pipe/pipe2`, `dup*`, `fcntl`
  (`F_DUPFD[_CLOEXEC]`, `F_GETFD/SETFD/GETFL/SETFL`), `ioctl` (+ `FIOCLEX`),
  `fsync`, `truncate`, `statfs`, `umask`, `uname`, `sysinfo`, `poll`, the clocks,
  `nanosleep`, the id calls, `setpgid/getpgid/setsid/getsid`, `prctl`,
  `sched_getaffinity`, `sched_yield`, `kill`, `fork`, `execve`, `exit`, `brk`,
  `munmap`, `mprotect` (W+X refused with `EACCES` first).
- **Converting**: `stat/lstat/fstat/newfstatat` (`struct secos_stat` →
  Linux `struct stat`), `rt_sigaction` (one handler + one process-wide restorer
  in the kernel; `sa_mask`, the flags and the old action are kept in a
  per-process shadow table), `rt_sigprocmask`/`rt_sigsuspend` (SecOS masks:
  bit n = signal n, Linux: bit n-1; signals 1..31 only), `getcwd` (Linux counts
  the NUL), `setreuid/setregid` and `getres[ug]id` (on the SecOS triples),
  `mount` (flags → leading `ro`/`remount` option tokens, as BusyBox does),
  `reboot` (Linux magic → SecOS command), `gettimeofday`, `time`.
- **Emulated**: `wait4` (`SYS_WAIT` for one pid, `SYS_WAITANY` for any child
  and the options; a child collected while waiting for another is stashed),
  `getdents64` (below), `readv/writev`, `pread64/pwrite64` (seek, I/O, seek
  back), `select/pselect6/ppoll` (on `poll`), `clock_nanosleep` (incl.
  `TIMER_ABSTIME`), `clock_getres`, `getrandom` (`/dev/urandom` = the kernel
  CSPRNG), every `*at()` call: the directory fd's path comes from
  `/proc/self/fd/N`, `O_DIRECTORY` and `O_NOFOLLOW` are checked with
  `fstat`/`lstat`, `unlink` refuses a directory (`EISDIR`), `mkdir` maps a bare
  failure to `EEXIST`/`EACCES`, `utimensat` (`UTIME_NOW/OMIT`, whole seconds,
  fd form for `futimens`).
- **`getdents64`** over the path-based `SYS_GETDENTS`: the path from
  `/proc/self/fd/N`, the position (entry index) is the fd's own file offset,
  so `rewinddir`/`seekdir`/`telldir` work and the position survives dup, fork
  and exec. `.` and `..` come first; `d_ino` and `d_type` come from an `lstat`
  of each entry (the kernel records carry only d/l/f).
- **Hints answered with success** (valid answers, not fakes): `madvise`
  (except `MADV_DONTNEED`, which must zero), `fadvise64`, `msync` (no shared
  file mappings exist), `mlock*` (no swap).
- **`mmap`**: anonymous and file `MAP_PRIVATE` pass through; `MAP_FIXED` only
  for what mallocng does (a `PROT_NONE` guard over pages already mapped =
  `mprotect`); a file offset only for a device (fb0); everything else that the
  kernel would get wrong is `-ENOSYS`.
- **Missing** → `-ENOSYS`, reported with `SECOS_SYSCALL_TRACE=1`:
  `secos-musl: ENOSYS linux syscall <nr> (<what>)`. Calls that work only partly
  print `secos-musl: partial ...` under the same switch.

### `tools/secos-cc`

Host gcc + the sysroot: `-nostdinc -isystem <sysroot>/include` (+ gcc's own
intrinsics headers), `-fPIE -D__secos__`; the link is static-PIE
(`rcrt1.o crti.o crtbeginS.o … -lc -lgcc -lgcc_eh crtendS.o crtn.o`),
`-z max-page-size=4096` (the loader's `p_align` rule), `-z noexecstack`,
`-z stack-size=8M`, plus the default manifest note (`lib/secos-note.o`, from
`user/note.S`: ambient user, no limits); then `tools/secos-sign` with the dev
key. `--secos-note=` picks another manifest (object or `.S`),
`--secos-unsigned` skips signing, `--secos-key=` signs with another seed.
Compile-only invocations (`-c/-S/-E/-M`) just add the include flags, so it
works as `CC` for a configure script.

## Gate MUSL-BASE (`tools/musl_base_test.sh`, in `tools/selftest.sh`)

Host: the sysroot builds and `libc.a` has no raw syscall; `secos-cc` builds
`user/musltest/mtest.c` signed, plus an unsigned copy, a copy with one byte of
`.text` flipped, and an `-z execstack` build; the ELF is a static-PIE without
`PT_INTERP` and with an 8 MiB RW `PT_GNU_STACK`; the signatures verify (or not)
host-side.

Guest (ISO `CONSOLE=root`, no root disk, the binaries on an ext2 disk at
`/mnt`, `-smp 4`, `-cpu max` = SMAP on, KASLR on): `mtest all` runs 32
self-checking tests, each a gate line — printf incl. `%e/%g/%a` and exact
`%.17g`, scanf with floats, strtod, libm, `__thread`, errno, the auxv, C.UTF-8
`mbstowcs`/`wcwidth`, iconv UTF-8↔Latin-1 both ways, qsort/bsearch (20000),
malloc stress (100000 mixed operations with content checks, realloc, calloc),
setjmp/longjmp, a signal handler round trip (handler sees its TLS; blocked
then delivered on unblock; FPU state intact), sigset API, fork + pipe + wait
(the child inherits TLS), fork + execve with argv/envp, posix_spawn (+ file
action) and its ENOENT, a child killed by a signal, ECHILD, stdio files,
POSIX files (`O_EXCL`, pread, rename, symlink/readlink/lstat), opendir with
`.`/`..`/types and rewinddir, `/bin` listing, chdir/getcwd and relative paths,
unlink/rmdir, nanosleep + CLOCK_MONOTONIC, uname, getentropy, mmap/mprotect/
munmap and W+X refused, 2 MiB of recursion on the 8 MiB stack. Then: the PIE
base and the TLS block differ between two runs and lie in [4, 10) GiB; six
concurrent processes keep their `__thread` value over 4000 yields each (the FS
base across switches and CPUs); a program started by `SYS_SPAWN` through
libsecos (kernel-made stack and auxv, no environment); the ENOSYS trace line
for `inotify_init1`, and silence without the variable; the unsigned and
tampered binaries are refused by the signature gate (rc 126, `[SEC] REFUSED`),
the signed exec-stack one by the loader; old-libc BusyBox, lua and bash still
run; no panic, SMAP violation, TLB bug or lockdep report. 54 checks.
`--lockdep` repeats it on a `-DSECOS_LOCKDEP` image.

## Tests run (at the end of U1)

| Run | Result |
|---|---|
| `tools/musl_base_test.sh` (default image, `-smp 4`, `-cpu max`: SMAP + KASLR on) | 55/55 |
| `tools/musl_base_test.sh --lockdep` (`-DSECOS_LOCKDEP` image) | 56/56, no lockdep report |
| `mtest all` with `SECOS_SYSCALL_TRACE=1` | no ENOSYS or partial call in the whole workload |
| selftest sections M16, M19, M30, M31/M32, M46, M39 POSIX + ASLR, M45 (MB2 + UEFI KASLR), M36/M37/M44 default image, M39 bash boot + bash regressions (cmdsubst, jobs, execve, line editing), M44 SMAP violation + uaccess/ABI | 220/220 |
| selftest sections M43 account tools, PERM, MUPORTS (set-id exec: the auxv creds patch) | 69/69 |
| `tools/smoke.sh --mb2` / `--uefi` | PASS / PASS |
| warnings (`make`, `make CFLAGS_EXTRA=-DSECOS_LOCKDEP`), base 0f5ba0d vs U1 | 0 / 0 vs 0 / 0 |

## Deviations from the plan

- **Layout window.** D4 said "any layout inside the user half above 64 KiB".
  The window is [4 GiB, 10 GiB) instead: below 4 GiB is still refused by
  `user_range_valid()` (`USER_ADDR_MIN`, a defence-in-depth floor since M44),
  and the page-table walkers (`vmm_space_destroy`, `vmm_fork_space`,
  `vmm_count_user_pages`, the COW fault, the CR3 audit) cover PML4[0] PDPT
  slots 4..15 only. A static-PIE needs no low addresses, and `secos-cc` only
  makes static-PIEs; a classic `-no-pie` link at 0x400000 is refused. Opening
  the rest of the user half belongs with the mmap allocator rework (U4a).
- **musl is fetched**, not committed (`third_party/musl/` is git-ignored), like
  bash/BusyBox; the plan said "vendored". The overlay is in the tree.
- **musl 1.2.6** (latest release) **+ two upstream security patches**: the
  musl site lists CVE-2026-6042 (iconv) and CVE-2026-107891 (regcomp) as
  affecting every release through 1.2.6. They are upstream commits from the
  musl list, pinned by SHA-256; musl itself is otherwise unmodified.
- **posix_spawn** is musl's own code with one change (fork instead of
  `clone(CLONE_VM|CLONE_VFORK)`), as a patch rather than a rewrite.
- **AT_EXECFN** of a spawned (not exec'd) program is its `argv[0]`.

## Linux calls musl wanted that the kernel lacks (worklist for U2/U3)

From the table and the gate runs with `SECOS_SYSCALL_TRACE=1`:
- `-ENOSYS`: `rt_sigpending`, `rt_sigtimedwait`, `rt_sigqueueinfo`,
  `sigaltstack`, `getrlimit/setrlimit/prlimit64`, `getrusage`, `times`,
  `alarm/setitimer/getitimer`, `statx` (musl falls back to `newfstatat`),
  `mremap` (realloc falls back to copy), `madvise(MADV_DONTNEED)`,
  `MAP_FIXED`/`MAP_FIXED_NOREPLACE`, anonymous `MAP_SHARED`, file maps at an
  offset, writable shared file maps, `fcntl` record locks (`F_GETLK/SETLK/
  SETLKW`) and other commands, `renameat2` flags, signals 32..64, every socket
  call (U3/U10), `clone`/`futex`/`set_robust_list` (U9), and everything else
  not in the table.
- **Partial** (work, not exactly as Linux): `rt_sigaction` (`sa_mask`,
  `SA_RESETHAND`, `SA_NODEFER`, `SA_ONSTACK`, `SA_SIGINFO` contents not
  honoured; a disposition inherited across exec reads as `SIG_DFL`), `wait4`
  (no rusage; process-group waits = any child), `faccessat` flags
  (`AT_EACCESS`), `O_PATH` (needs read permission), `pread/pwrite` (not
  atomic), `ppoll/pselect6` (mask not atomic), `getdents64` and every `*at()`
  call (path-based underneath), `utimensat` (seconds), bad user pointers fault
  in the process instead of `EFAULT`.
- Kernel limits musl programs will meet: 32 fds, 64 VMAs per process (mallocng
  maps a group per medium/large size class: a program with many live
  allocations of many sizes can run out — U4a), the mmap bump allocator (no
  address reuse, ~3.75 GiB per process lifetime — U4a), 16 MiB exec images.

## Notes for U2/U3

- New kernel calls: 512 + the Linux number, Linux-shaped, dispatched by
  `syscall_lx_dispatch()`; add them to `lx_lockfree()` in `kernel/bkl.c` once
  they are safe without the BKL. Then the translation layer entry becomes
  `sc(S_LX + n, ...)`, and the emulation goes.
- The layer's `getdents64` and `*at()` emulation rely on `/proc/self/fd/N`
  returning the inode's path; U2's fd-based calls replace them.
- `struct secos_stat` lacks nanoseconds: `stat_conv()` zeroes them.
- With threads (U9): `syscall_cp.s` calls the layer, so the pc of a thread
  blocked in a syscall is not inside `[__cp_begin, __cp_end)`; asynchronous
  cancellation needs rework there. `clone.s`/`__unmapself.s` are stubs.
- `AT_HWCAP2` is 0 and `CR4.FSGSBASE` stays off; enabling it later means
  saving the user FS/GS base at switch-out (user space could change it).
- The ABI tests and old programs still use `int $0x80` with `rcx` as the 4th
  argument; musl programs do too (through the layer). A `syscall`-instruction
  entry is a separate decision (the plan's performance note).
