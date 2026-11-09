# Userland plan for SecOS (U1–U14)

Written from main `c8d41db` (M0–M52 done, default `bkl=off`; Phase 5 M53a–c
running on `integ/phase5`) by a planning agent. File and line references are
for that commit. The design decisions are recommendations. The ones marked
**DECIDE** need the user's answer before the step that depends on them starts.
Implementers must check every claim against the code.

The user's order of wishes:
1. a much richer C library ("not everything glibc has, but almost"), the
   foundation for everything after;
2. GNU nano (desktop terminals and SSH) and a graphical task manager in the
   style of Windows' (process list with CPU/mem/user and "End task", per-core
   CPU and memory graphs, maybe network);
3. OpenSSL, GNU wget (HTTPS), curl;
4. a native toolchain (gcc/g++, binutils, make, cmake, …);
5. maybe Python;
6. later a browser: links (text) first, then NetSurf (framebuffer). Only the
   prerequisites are planned here.

What shapes this plan:
- **The libc question is mostly a kernel question.** Whatever libc SecOS uses,
  gcc, cmake, Python and OpenSSL need things the kernel does not have:
  threads, a thread pointer (FS base), an auxiliary vector, fd-based
  `getdents`, the `*at()` calls, more than 32 processes / 32 fds / 64 VMAs,
  executables bigger than 16 MiB, and an ELF layout that a normal toolchain
  produces. About half of the work in this plan is kernel work, and it is the
  same under every libc option.
- **The recommended libc is musl, ported in user space (option B1).** musl's
  Linux syscalls are translated to the SecOS ABI by a small layer inside libc.
  The kernel ABI stays custom (the locked decision), the M3 user-pointer
  contract stays in one place (the kernel), and the existing ports keep running
  on the old libc until each one is rebuilt.
- **Programs reach the disk by being compiled into the kernel image**
  (`kernel/sysroot.c`). That does not scale past a few MB per program, and a
  toolchain is hundreds of MB. Package delivery has to change before the
  toolchain, Python or the browser.
- **"Only signed code runs" meets "SecOS compiles its own code".** A native
  toolchain only makes sense if a program built on the machine can run. How it
  gets signed is a security policy choice for the user (§2 D9).
- **nano and the task manager can start now.** The task manager is an LVGL
  client like `/bin/sysmon` and needs almost nothing new. nano's port recipe can
  be developed against the host's musl while the SecOS musl is brought up.

---

## 1. Current state

### 1.1 The C library
| Piece | Where | State |
|---|---|---|
| Raw syscall + POSIX-ish wrappers | `user/libsecos.c` (364 lines), `user/libsecos.h` (183) | `int $0x80`, 5 args (`libsecos.c:47-50`), negative return → errno |
| Standard C | `user/libc.c` (1409 lines) | printf core (incl. `%f/%e/%g`), buffered `FILE`, string/stdlib/ctype, malloc (first-fit over `sbrk`), qsort, creds, fcntl, getdents, sysconf |
| Math, setjmp | `user/libm.c`, `user/setjmp.S` | Enough for lua 5.4 |
| crypt | `third_party/musl-crypt` (musl 1.2.5, MIT, unmodified), `user/libcrypt.a` | SHA-512 crypt for login |
| Headers | `user/include/` (≈ 30 headers, most under 60 lines) | Linux values for errno, `O_*`, signals, termios; no `wchar.h`, `pthread.h`, `netdb.h`, `regex.h`, `locale` beyond a stub |
| Startup | `user/crt0.S` | argc/argv/envp passed in `rdi/rsi/rdx`; also the `__sigreturn` trampoline |
| Link layout | `user/user.ld`, `-mcmodel=large` (`Makefile:368`) | Text at 0x1_0000_0000, data at 0x2_0000_0000 (required by the loader, §1.5) |

The ports add their own libc pieces, one shim per port:
| Port | Shim | Notes |
|---|---|---|
| BusyBox 1.36.1 | `user/busybox_compat.c` (2061 lines), `user/busybox_port.h`, `user/busybox-include/` (≈ 45 headers incl. a `musl/` subset) | BSD sockets, `getaddrinfo` (`busybox_compat.c:1470`, reads `/etc/resolv.conf` at :1291), regex/glob/fnmatch from musl sources |
| bash 5.2 | `user/bash_compat.c` (167), `user/bash_port.h` (203), `user/port-include/` | |
| Dropbear | `user/dropbear_compat.c` (508), `user/dropbear_port.h` (525), `user/dropbear-include/` | |
| lua 5.4.7 | `user/lua_port.h` (47) | |
| LVGL 9.2.2 | `tools/port-lvgl.sh`, `user/lvgl/lv_conf.h` | Clients: `/bin/sysmon` |

So SecOS already carries about 3,500 lines of per-port libc, three partial
copies of the BSD socket API and its own `getaddrinfo`. Every new port would
add another shim.

### 1.2 The syscall ABI
- `kernel/syscall.h`: 93 numbers in use. 0–61, 65–80, 85–86, 90–91, 95,
  100–109. Free: 62–64, 81–84, 87–89, 92–94, 96–99, ≥ 110. **Phase 5 takes 92
  (`SYS_UDRV`)**. `TASKS.md` also reserves 92–94 for GUI shared memory, so the
  two plans collide (§1.8).
- Entry is `int 0x80` only (`arch/x86/syscall_asm.asm`); no `syscall`/LSTAR path.
- Path-based where Linux is fd-based: `SYS_GETDENTS(path, buf, len)`
  (`syscall.c:1035`), `SYS_STAT`, `SYS_CHMOD/CHOWN/UTIMES` (fd forms via a NULL
  path).
- Sockets are a custom API: `socket(type)`, `connect(fd, ip, port)`,
  `bind(fd, port)` (`libsecos.c:211-212`), `struct secos_sockaddr {ip, port}`.
  AF_INET only. 64 sockets (`net/socket.c:44`) and 64 TCP connections
  (`net/tcp.c:35`) system-wide. Non-blocking internally, `-EINPROGRESS`
  connect (M47). No `setsockopt` in the kernel (the shims answer 0), no
  `shutdown`, no `sendmsg/recvmsg`, no AF_UNIX, no `socketpair`.
- Constants already have Linux values: errno, `O_CREAT=0x40`, `O_NONBLOCK=0x800`,
  `O_CLOEXEC=0x80000`, `F_DUPFD_CLOEXEC=1030`, signal numbers 1–31, termios
  ioctls. That makes a translation layer cheap.

### 1.3 Processes, signals, threads
- **No threads.** No `clone`, no futex, no FS base (`grep` of `kernel/ arch/ mm/`
  finds no `FS_BASE`/`arch_prctl`; the kernel does not save or set MSR
  0xC0000100). `/proc/<pid>/status` prints `Threads: 1` (`fs/procfs.c:429`).
- `process_t` (`kernel/process.h:52-232`) holds everything a Linux task_struct,
  mm_struct, files_struct and sighand_struct hold, in one object: fpu, space,
  vmas, brk/mmap state, fds, cwd, creds, signals, wait state.
- `MAX_PROCESSES 32` (`kernel/process.c:26`) = `M5_MAX_KSTACK_SLOTS 32`
  (`mm/vmm.h:224`): 32 processes for the whole system.
- `fds[32]` (`process.h:192`), a fixed array.
- Signals: `NSIG 32` (`kernel/signal.h:19`), `sig_handler[32]`, 64-bit masks.
  The handler frame saves the FPU (`kernel/signal.c:273`; the `TASKS.md` item
  saying it does not is out of date). No `SA_SIGINFO`/`siginfo_t`, no
  `sigaltstack`, no real-time signals. musl needs signals 32–34 internally
  (SIGCANCEL, SIGSYNCCALL) once threads exist.
- The initial user stack is laid out like Linux (`process.c:546`: argc, argv,
  NULL, envp, NULL) but has **no auxiliary vector** (no `AT_PHDR`, `AT_RANDOM`,
  `AT_PAGESZ`, `AT_SECURE`). musl's startup reads it.
- Blocking = `sched_block_current` + rip rewind + re-run (M16/M17). Every wait
  kind has its own field in `process_t` (`wait_pipe`, `wait_tty`, `wait_pty`,
  `recv_chan`, …).
- `fork` is COW (M19) but **copies the pinned ELF image** with `kmalloc`
  (`process.c:1157`). `execve` is in place (Phase 2).

### 1.4 Memory and executables
| Limit | Where | Effect on the targets |
|---|---|---|
| Exec image ≤ 16 MiB | `SPAWN_MAX_IMAGE`/`EXEC_MAX_IMAGE`, `kernel/syscall.c:115`, `:242` | `cc1plus` is 30–45 MB static, `python3` 10–25 MB |
| Whole image `kmalloc`'d and pinned for the process lifetime | `process.c:626` | Needs contiguous kernel heap; RAM per running copy |
| Fixed user layout: code in [4 GiB, 8 GiB), data in [8 GiB, 16 GiB) | `mm/vmm.h:245-251`, loader checks `mm/elf.c:76-81`, `:168-170` | A normal toolchain links at 0x400000 or as static-PIE: **refused**. Every port uses `user.ld` + `-mcmodel=large` |
| mmap = bump allocator, no reuse, `addr` hint ignored, no `MAP_FIXED` | `syscall.c:427-471` | 3.75 GiB of mmap address space for the process lifetime. `gcc`'s GC, Python and musl's mallocng mmap and munmap all the time |
| `VMA_MAX 64`, a fixed array | `mm/vma.h:23` | musl mallocng and Python arenas create hundreds |
| `MAP_SHARED` only for device memory (fb0) | `syscall.c:427` | Python `mmap`/`multiprocessing`, POSIX shm |
| Page cache = 128 pages (512 KiB), FIFO | `mm/pagecache.c:45` | Compiling re-reads headers constantly |
| Pipe ring 4 KiB | `kernel/pipe.c:17` | Linux has 64 KiB; `gcc -pipe`, make jobserver, SWP bandwidth |
| `PROT_EXEC` on anonymous memory and `mprotect` RW→RX are **allowed** (only W+X at once is refused) | `syscall.c:444`, `:518-531` | A signed program can already run code it generates or loads (§2 D7) |

### 1.5 How programs reach the disk
- Every system program is a signed ELF compiled into the kernel image as a
  byte array (`crypto/user_*_elf.h`; bash ≈ 5.8 MB of header text), and
  `kernel/sysroot.c` reconciles them onto the root at boot (SHA-256 manifest
  in `/var/lib/secos/manifest`). `kernel.bin` is 5.4 MB.
- `.spkg` packages (M32, `kernel/pkg.c`): verified and unpacked **in the kernel,
  from a kernel buffer** holding the whole package. Fine for a motd. Not usable
  for a 400 MB toolchain.
- The persistent root is a 1 GiB sparse ext4 image (`tools/mkroot.sh`, size is
  a parameter) that is never recreated automatically.
- **Only one ext2/ext4 volume can be mounted at a time** (M41 gap; one
  `g_e2_lock`, global driver state). So a separate ext4 "SDK disk" is not
  possible today.
- The trusted key: one project key (`crypto/secos_pubkey.h`, DEV seed in
  the public tree), `key_id` in the QSIG note, a revocation list
  (`mm/elf_sign.c:47-74`). No keyring with more than one key yet.

### 1.6 What the task manager can use today
- `/proc/stat` has a total `cpu` line and one `cpuN` line per CPU
  (`fs/procfs.c:288-305`); `/proc/<pid>/{stat,status,statm,cmdline,comm,maps,fd,…}`
  (`procfs.c:97-101`); `status` has `Uid:` (`:415`) and `VmRSS:` (`:426`);
  `/proc/meminfo`, `/proc/loadavg`, `/sys/devices/system/cpu/*` (M48).
- **No `/proc/net/dev`** and no per-NIC byte/packet counters exposed.
- `/bin/sysmon` (`user/lvgl/sysmon.c`, 268 lines) is the template: LVGL in
  DIRECT mode, SWP over fds 3/4, a confined manifest
  (`user/note_confined.S`: `CAP_ENFORCE|CAP_FS_READ|CAP_TIME`), 1 Hz `FRAME`.
  Windows are at most 1024×720 (`user/secwm/swp.h:23-24`), and pixels travel
  through a 4 KiB pipe.
- `kill` permission is checked by `cred_may_signal` (`kernel/cred.c:283`). A
  task manager running as a user can end only that user's processes.

### 1.7 Network, time, entropy
- DHCP in user space (`/bin/ifup`, `SYS_NETCTL`), DNS server kept in the
  kernel's NIC config; **nothing writes `/etc/resolv.conf`**.
- The clock comes from the RTC at boot (`kernel/kclock.c`), `clock_settime`
  needs `CAP_TIME`. There is no NTP client (BusyBox `ntpd` is not configured).
- `/dev/urandom` = kernel CSPRNG (`fs/devfs.c:244`); no `getrandom` syscall.
- No CA certificate store.

### 1.8 Work running in parallel
Phase 5 (`docs/PHASE5_PLAN.md` on `integ/phase5`) touches `kernel/syscall.{c,h}`
(`SYS_UDRV` = 92), the fd table (a new IRQ fd type), `kernel/kiface.c` (poll)
and `process_destroy`. The kernel milestones below touch the same files.
`TASKS.md`'s reservation of 92–94 for GUI shared memory must move (§2 D3).

---

## 2. Design decisions

### D1. The C library: adopt musl, translate in user space (B1)

| | A: grow `user/libc.c` | **B1: musl + user-space translation** | B2: kernel Linux personality | B3: new SecOS calls take Linux numbers |
|---|---|---|---|---|
| What it is | Keep writing libc by hand | musl 1.2.x with its syscall layer replaced: `__syscall(SYS_x, …)` calls a SecOS table that converts arguments/structs and issues SecOS syscalls | A second syscall table in the kernel with Linux numbers and structs, entered via `syscall` (LSTAR), enabled per binary by a manifest flag; stock musl | Renumber so musl needs no table |
| Compatibility for gcc/cmake/Python/OpenSSL | Poor: these need ~50–80 kLOC of libc (locale, wide chars, iconv, regex, stdio corner cases, pthreads, resolver, `printf` exactness). Years of bugs | musl's full POSIX + C11 surface, the libc of Alpine: all targets are known to build on it | Same as B1, plus semantics exactly Linux's | Same as B1 for new calls only |
| Kernel ABI | Unchanged | **Unchanged, only additions** (the locked decision holds) | Two ABIs forever | Partly Linux; 0–3, 56 (clone vs `FSTAT`), 61 (wait4 vs `FCNTL`) … collide, so it can never be complete |
| Security | Fine | The translation layer runs in ring 3 inside the process. A bug there hurts only that process. The M3 contract stays in the kernel, in one dispatcher, with the same `cap_check` gates | A second ~200-entry syscall surface to keep M3-hardened, cap-gated, in the BKL lists and lockdep. Every Linux flag becomes a kernel promise | Neutral |
| Cost | Endless | ≈ 4–6 kLOC SecOS code in musl (`src/secos/`), plus the kernel work every option needs | Large kernel work, then the same kernel features as B1 | Small, but buys little |
| Migration of existing ports | Shims stay | Rebuild each port; shims mostly disappear (BusyBox's 2061-line shim shrinks to almost nothing) | Same | — |
| Runs Go/raw-syscall binaries | No | No (Go issues Linux syscalls itself) | Yes, if signed | No |
| Overhead | — | One indirect call + occasional struct copy per syscall; negligible next to `int 0x80` | Lowest | — |

Other libcs considered: glibc (LGPL, tied to the Linux kernel and dynamic
linking, huge), newlib/picolibc (embedded, little POSIX), bionic (Linux-tied),
FreeBSD libc (BSD ABI), relibc (Redox does exactly B1, but in Rust). musl is the
fit: MIT (it can live in-tree, like `third_party/musl-crypt`), static-first,
small, readable, already partly used by the BusyBox port.

**Recommendation: B1.** And every *new* kernel call is designed **Linux-shaped**
(same argument order, struct layout, flags and errno as the Linux call it
serves), so its translation is a pass-through. That also keeps B2 cheap if it
is ever wanted (for Go, say), without committing to it now. **DECIDE (Q1).**

### D2. How musl is ported
- **Source:** musl 1.2.5 (or the newest release when U1 starts, pinned by
  SHA-256), vendored in `third_party/musl/` unmodified, with SecOS changes as an
  overlay applied by `tools/port-musl.sh` (like BusyBox's `bb_patch_src.py`), so
  upstream updates stay mechanical.
- **Syscall layer:** `arch/x86_64/syscall_arch.h` (`__syscall0..6`) calls
  `__secos_syscall(long n, long a, …)` in a new `src/secos/` directory. It is a
  `switch` on the Linux number:
  - direct (`read`, `write`, `close`, `lseek`, `dup`, …): renumber only;
  - converting (`fstat`/`stat`/`newfstatat` ← `struct secos_stat`;
    `getdents64` ← kernel records; `socket/connect/bind/sendto/…` ←
    `sockaddr_in` ↔ `secos_sockaddr`; `rt_sigaction` ↔ `SYS_SIGACTION` with
    SecOS's restorer; `clone` → SecOS thread call; `wait4` → `SYS_WAITANY`);
  - emulated (`posix_spawn` → `fork`+`execve` until a spawn-with-actions call
    exists; `pselect6`/`select` → `poll`; `sched_*` → `sched_getaffinity`/`yield`);
  - unsupported → `-ENOSYS`, counted. A `SECOS_SYSCALL_TRACE=1` environment
    variable logs every `-ENOSYS` to stderr: the porting worklist.
- `__restore_rt` (rt_sigreturn trampoline) and `__set_thread_area` are
  rewritten for SecOS; `vfork` = `fork`.
- **SecOS extras** (`spawn`, `getticks`, `msg_send/recv`, `netctl`, `prctl`
  SecOS options, raw `secos_syscall`) move to a small `libsecos.a` with a
  `<secos/secos.h>` header, linked by the programs that use them (secwm, init,
  netcfg, tests).
- **Compiler driver:** `tools/secos-cc` (and `secos-c++` later) = host gcc +
  `-nostdinc -isystem <sysroot>/include`, `-static-pie`, musl's
  `rcrt1.o/crti.o/crtn.o`, `libc.a`, `libgcc.a`, the default manifest note
  object, then a post-link `secos-sign` step. Ports call it as `CC`. In U12 it is
  replaced by a real cross toolchain built with the same sysroot.
- **Target triple:** `x86_64-linux-musl` for the cross and native gcc, with
  `-D__secos__` in the specs. Every autoconf `config.sub` knows it, and the
  ports' configure scripts then assume Linux, which is what the translation
  layer imitates. The alternative `x86_64-secos-musl` is more honest but
  means patching `config.sub`/`config.guess` in every package. **DECIDE (Q3).**

### D3. Numbering new syscalls
musl's table maps numbers anyway, so the number is an internal choice. Options:
- (a) next free numbers (110, 111, …);
- (b) **`512 + Linux number`** for every Linux-shaped call (`futex` = 714,
  `openat` = 769, `getrandom` = 830). The number says which Linux call it
  mirrors, the musl table entry for it is a constant offset, and SecOS-only
  calls keep the low range. A future B2 entry point would be "subtract 512";
- (c) the Linux number itself where SecOS's slot is free (≥ 110): impossible for
  `clone` (56) and others, so it would be a mix.
**Recommendation (b)**, with the dispatcher using a second, sparse table for
512+. GUI shared memory (`TASKS.md` 92–94) moves to `memfd_create` (512+319) and
`MAP_SHARED`, which U4 adds anyway, leaving 92 to Phase 5. **DECIDE (Q2).**

### D4. Executable format and address space
- The loader accepts **any** `PT_LOAD` layout inside the user half above
  64 KiB, plus **ET_DYN without `PT_INTERP`** (static-PIE), loaded at a random
  base. Then the image is ASLR'd too (today only stack/heap/mmap slide, M39),
  and the small code model works (`-mcmodel=large` is slower and bigger).
  `user.ld` binaries keep loading (they are a valid layout).
- `PT_TLS` is handled by musl; the kernel only provides `AT_PHDR`.
- `PT_GNU_STACK` with X is refused (W^X); `PT_GNU_RELRO` is honoured by musl's
  static-PIE self-relocation (`rcrt1`: relocate, then `mprotect` RO).
- auxv: `AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_ENTRY, AT_BASE=0, AT_UID,
  AT_EUID, AT_GID, AT_EGID, AT_SECURE (set-id exec), AT_RANDOM (16 CSPRNG
  bytes), AT_HWCAP/AT_HWCAP2, AT_CLKTCK, AT_EXECFN, AT_MINSIGSTKSZ`.
- The user address space becomes Linux-like: mmap allocates **top-down with
  reuse** (a free-range tree), honours hints and `MAP_FIXED`/`MAP_FIXED_NOREPLACE`,
  `mremap`, and the VMA set becomes a dynamic sorted structure (no 64 cap).
  Manifest `max_mem` keeps bounding the sum (M13/M18).

### D5. Threads: Linux task model (shared, refcounted parts)
Two ways to change `process_t`:
- (T1) split into `proc_t` (address space, fds, creds, signal handlers) and
  `thread_t` (kstack, trapframe, FPU, state, wait fields, mask, FS base),
  with the scheduler switching threads; or
- (T2) **keep `process_t` as the schedulable task** and move the shareable
  parts into refcounted objects that a thread shares: `mm_t` (space, vmas,
  brk/mmap state, image, `mm_lock`), `files_t` (fd table, cwd, umask),
  `sighand_t` (handlers, restorer). A thread group shares a `tgid`.

**Recommendation T2.** It lands in steps that each change no behaviour (first
the three objects with 1:1 sharing, then `clone`), keeps the M48 scheduler and
the M50–M52 locking (which are per-`process_t` and freshly validated), and
matches what musl's `pthread_create` asks for (`CLONE_VM|CLONE_FS|CLONE_FILES|
CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|
CLONE_CHILD_CLEARTID`). Only that combination (and the `fork` one) is
accepted; anything else is `-EINVAL`.

The rest of the threads work:
- **Thread pointer:** `arch_prctl(ARCH_SET_FS/GET_FS)`; FS base saved/restored
  in `switch_to` (MSR, or `wrfsbase` when `CR4.FSGSBASE` is on), inherited by
  fork, reset by exec. Needed already in U1: musl sets it up in every program.
- **futex:** `WAIT`, `WAKE`, `REQUEUE`, `CMP_REQUEUE`, `WAIT_BITSET`, private
  flag, `CLOCK_REALTIME` flag, timeouts. Hash on (mm, uaddr) for private
  futexes; shared futexes key on the physical frame. Built on the existing
  block + re-run: a wake sets a per-task `futex_woken` so the re-run returns 0
  instead of re-checking the value (otherwise a wake with the value unchanged
  would be lost).
- `set_tid_address`, `CLONE_CHILD_CLEARTID` (zero + futex wake at exit: this is
  `pthread_join`), `exit` (thread) vs `exit_group`, `gettid`, `tgkill`,
  `getpid` = tgid, wait/SIGCHLD only on group exit, `execve` from a thread kills
  the others, `fork` from a thread copies only the caller.
- **Signals:** `NSIG` 64 + per-thread mask and pending set, process-directed
  pending delivered to any thread that does not block it, stop/continue of the
  whole group, `SA_SIGINFO` + `siginfo_t` + `ucontext_t`, `sigaltstack`.
- **Credentials:** process-wide (one `cred` per thread group). musl's
  `__synccall` for `setuid` then still works, but is not needed.
- **Limits:** dynamic process table and kernel stacks (no 32 cap), per-user
  `RLIMIT_NPROC`.
- `/proc/<pid>/task/<tid>/…`, `Threads:` real, the task manager shows threads.
- `sched_setaffinity` (the mask already exists for get, M48).

Who needs threads: CPython ≥ 3.7 (mandatory), cmake (libuv + `std::thread`),
libstdc++'s `std::thread`/`<mutex>` for user C++, OpenSSL 3 by default (but
`no-threads` is supported), curl's threaded resolver (optional). Not needed:
nano, ncurses, wget 1.x, binutils, make, gcc's own binaries (gcc builds with
`--enable-threads=single`), links, NetSurf. So gcc, OpenSSL, curl and wget come
**before** threads, and are rebuilt with threads after U9.

### D6. Exec from the page cache, and verify-once
Today exec copies the whole file into `kmalloc` and keeps it. New path:
- verify the signature while streaming the file (SHA-256 over page-cache
  pages, no full copy);
- FILE VMAs reference the **inode + offset** in the page cache (M20 already
  maps cached pages privately);
- the file is **write-locked while it is mapped for exec** (`ETXTBSY` on open
  for write/truncate), so the verified bytes cannot change under a running
  process (the TOCTOU the pinned copy prevents today);
- a **verification cache** keyed on (device, inode, generation/ctime, size)
  avoids re-hashing a 40 MB `cc1plus` at every compile (SHA-256 in C runs at
  roughly 200–400 MB/s, so 0.1–0.2 s per exec otherwise). An entry is dropped
  on any write.
- fork no longer copies the image (it shares the inode reference).
- The page cache becomes RAM-sized with LRU reclaim (U4b), and the exec image
  limit becomes "fits the address space and `max_mem`".

### D7. Static only, and executable memory
- **No dynamic linking.** `dlopen` returns an error (musl's static `dlopen` stub).
  Python extension modules are built in (`Modules/Setup.local *static*`),
  OpenSSL is `no-shared no-dso`, gcc is `--disable-plugin --disable-lto` (or LTO
  without the linker plugin). Shared libraries would need the kernel to verify
  every `PROT_EXEC` file mapping, which is a later design.
- **Executable anonymous memory.** Today a signed program may `mprotect` its
  own data to RX (§1.4). That is how a loader or a JIT runs code nobody signed.
  The signature then covers only the first native code that runs. Recommendation:
  **forbid** `PROT_EXEC` on anonymous/`MAP_PRIVATE`-written memory by default,
  allow it only with a signed manifest bit `MANIFEST_FLAG_EXECMEM` (audited).
  Nothing in the current tree or in the U-targets needs it: libffi 3.4+ uses
  static trampolines on x86-64 Linux (`FFI_EXEC_STATIC_TRAMP`), Python's JIT is
  off by default. **DECIDE (Q5).**
- Interpreters remain a known gap: bash, lua and Python run unsigned scripts.
  That is the usual model (file permissions protect `/usr/lib/python3*`) and is
  written down, not changed.

### D8. Delivering large software
- **Bigger root:** new images default to 8 GiB sparse (it costs nothing on the
  host). `make grow-root SIZE=…` runs `resize2fs` offline on an existing image,
  then `e2fsck -fn`. The SecOS ext4 driver must accept the result: a gate,
  not an assumption (the M41 feature whitelist and flex_bg/resize_inode
  handling).
- **Packages v2:** a user-space installer `/bin/spkg` (root, signed, `CAP_FS_*`):
  streams a `.spkg` from a file or URL, verifies its signature with an Ed25519
  verify compiled into the installer (the same digest rule), extracts into a
  staging directory, renames into place, records owned files in
  `/var/lib/spkg/` (remove/upgrade). Executables inside are still verified at
  every exec, so the package signature protects headers, `.a` libraries, the
  Python stdlib and data — the files exec never checks.
- **Transport:** a host-built transfer disk (`make pkgdisk` → exFAT image,
  attached as SATA/virtio, mounted at `/mnt`) first, HTTP(S) once wget exists
  (U11), and the same `.vmdk` route for VMware.
- The kernel keeps embedding the boot-critical set (init, login, bash, BusyBox,
  Dropbear, secwm, the account tools). Everything else (nano, taskmgr, curl,
  wget, OpenSSL tools, the SDK, Python) becomes a package.
- Multi-instance ext4 (an RO SDK volume) is the alternative to a bigger root.
  It is L-sized work in `fs/ext2.c`. Not recommended now. **DECIDE (Q6).**

### D9. Signing programs that SecOS compiles itself
The native toolchain is only useful if its output can run. Options:
- **S0. No on-device execution.** The toolchain exists, but a binary it builds
  runs only after the developer copies it to the build host for signing. Keeps
  the model pure; makes self-hosting mostly pointless.
- **S1. Developer mode (unsigned, confined).** A boot option or `/etc` setting
  lets unsigned ELFs run if they are owned by a user in group `dev`, in an
  allowed directory, and **always** with `no_new_privs`, the ambient user
  capabilities minus `CAP_NET`/`CAP_INPUT`/`CAP_DISPLAY`/`CAP_POWER`, never
  set-id, never driver. Simple, but an unsigned program can then run.
- **S2. A local machine key with a capability ceiling (recommended).** At first
  boot SecOS generates an Ed25519 key pair. The kernel keyring gets the public
  half as `key_id` 1, class **local**, with a ceiling: no `DRIVER`, no `SETID`,
  no `SESSION_GATEWAY`, explicit caps (`NET`, `INPUT`, `DISPLAY`, `POWER`,
  `EXECMEM`) only if the owner of the key allows them per signature. The
  private half is readable only by a small root-owned signing service
  (`/sbin/secos-signd`, or a set-id `secos-sign` helper) that signs for members
  of `dev`/`wheel` after a password prompt (like `sudo`), logs every signature
  to `/var/log/auth.log`, and refuses manifests above the ceiling. The
  toolchain's link step calls it (`ld` wrapper / `-Wl,--secos-sign`). Every
  local binary still runs signed, can be revoked (revoke key 1 → all local
  builds stop), and can never become a driver or set-id.
- **S3. Per-user keys enrolled by root.** Like S2, one key per developer,
  `/etc/secos/keys.d/`. More bookkeeping; useful on a multi-user machine.
- **S4. Sign with the project key on the device.** Rejected: the project key
  must never be on a machine.

Kernel work for S2/S3 (U12a): a keyring (`key_id` → public key, class, ceiling),
loaded from the root at boot and **sealed** after init starts (a key can be
added only by a signed root tool, with an audit line), plus the ceiling check
in `process_create_from_elf`. **DECIDE (Q4).**

### D10. TLS trust: certificates, time, entropy
- CA bundle: Mozilla's set (curl's `cacert.pem`, MPL-2.0 data) at
  `/etc/ssl/certs/ca-certificates.crt`, shipped and updated as a signed package.
- Time: BusyBox `ntpd -n -q` (step only, through `clock_settime`; the kernel
  gains no `adjtimex`, the shim answers it read-only) run by init after `ifup`,
  plus `hwclock -w`. Off-line machines keep the RTC (UTC). Certificate checks
  fail loudly when the clock is wrong. **DECIDE (Q13)** whether time sync is on
  by default and which server.
- `getrandom` (Linux-shaped) on the CSPRNG; `AT_RANDOM`.
- `ifup` writes `/etc/resolv.conf` from the DHCP lease; musl's resolver
  (UDP, then TCP fallback) replaces the shims' `getaddrinfo`.

### D11. The task manager
- `/bin/taskmgr`, an LVGL client like `sysmon` (same glue), signed with a
  confined manifest `CAP_ENFORCE|CAP_FS_READ|CAP_TIME|CAP_SIGNAL`.
- Tabs: **Processes** (name, PID, user, CPU %, memory, state, threads; sort by
  column; "End task" = SIGTERM, then SIGKILL after 3 s if still alive, with a
  confirmation), **Performance** (one CPU graph per core from `/proc/stat`, a
  memory graph from `/proc/meminfo`, a network graph per NIC from
  `/proc/net/dev`, uptime), **Users** (sessions grouped by uid).
- Kernel: `/proc/net/dev` with per-NIC counters (new), `/proc/<pid>/stat`
  `utime/stime` already there; later `Threads:` real (U9).
- Ending another user's process is refused by `cred_may_signal`; the UI says
  so. A privileged "end as administrator" would need a GUI elevation flow,
  which SecOS does not have. **DECIDE (Q9).**
- The window stays ≤ 1024×720; graphs redraw only their damage, so the 4 KiB
  pipe is enough at 1 Hz.

### D12. nano
- ncurses 6.5 (MIT-X11) as `libncursesw.a`, `--with-fallbacks=linux,xterm,
  xterm-256color,vt100,screen` (terminfo entries compiled into the library: no
  terminfo database to install), `--disable-db-install`, widechar on.
- GNU nano 8.x (GPL-3.0, fetched by `tools/port-nano.sh`, never copied into
  the tree, like bash), `--disable-libmagic`, syntax files installed under
  `/usr/share/nano`, `/etc/nanorc` with `include`. `LANG=C.UTF-8` in
  `/etc/profile` so ncursesw and the fb console agree on UTF-8.
- It needs from libc: wide chars, `regcomp`, `nl_langinfo`, `getopt_long`,
  `glob`, `fnmatch`, `SIGWINCH`, termios raw mode, `TIOCGWINSZ`. All of them come
  with musl. On the old libc this would be another large shim. **Recommendation:
  build nano on musl** (U8 right after the U3 subset it needs). Start the
  recipe earlier against the host's `musl-gcc`, where the binary runs on the
  build host for a quick check. **DECIDE (Q8).**

---

## 3. Milestones

Sizes are agent-time: **S** ≤ 1 week, **M** 1–3 weeks, **L** 3–6 weeks,
**XL** > 6 weeks. Every milestone has a devlog `docs/devlog/U<n>-<topic>.md`
and, unless it says otherwise, keeps every existing selftest gate green.
Default boot behaviour changes only where a milestone says so.

| # | Branch | What | Needs | Size |
|---|---|---|---|---|
| U1 | `feat/musl-base` | musl bring-up + kernel minimum (auxv, FS base, standard ELF layout / static-PIE) | — | L |
| U2 | `feat/posix-kernel` | Kernel POSIX surface: fd-based calls, `*at`, signals 64 + siginfo, rlimits, timers, `getrandom` | U1 kernel part | L |
| U3 | `feat/musl-core` | Translation completeness + musl's libc-test as a gate | U1, U2 (incremental) | L |
| U4a | `feat/kernel-scale` | Dynamic process table / kstacks / fds / VMAs, mmap allocator, anon `MAP_SHARED`, `memfd`, 64 KiB pipes, exec-memory policy | U1 | L |
| U4b | `feat/exec-pagecache` | Exec from the page cache, `ETXTBSY`, verify cache, RAM-sized page cache | U4a | L |
| U5 | `feat/taskmgr` | Task manager on the current libc + `/proc/net/dev` | — (start now) | M |
| U6 | `feat/pkg2` | Package installer v2, transfer disk, bigger root | — (start now) | M |
| U7 | `feat/musl-ports` | BusyBox, bash, Dropbear, lua, secwm/LVGL clients, SecOS tools on musl; old libc retired | U3, U4a | L |
| U8 | `feat/port-nano` | ncurses + nano | U3 subset (recipe from U1) | S–M |
| U9 | `feat/threads` | clone/futex/TLS/thread groups/per-thread signals; musl pthreads | U4a, U2 | XL |
| U10 | `feat/net-posix` | Sockets: AF_UNIX, sockopts, shutdown, msg calls, `epoll`; resolv.conf, NTP, CA store | U3 | L |
| U11 | `feat/port-tls` | OpenSSL 3.5 LTS, curl, GNU wget (no threads; rebuilt after U9) | U10, U6 | M–L |
| U12a | `feat/port-toolchain` | Cross + native binutils, gcc (C/C++), make, SDK package, on-device signing (D9) | U4b, U6, U7, Q4 | XL |
| U12b | `feat/port-cmake` | cmake, ninja (or samurai), pkgconf; gcc rebuilt with posix threads | U12a, U9, U11 | L |
| U12c | `feat/port-devtools` | perl, autoconf/automake, git, patch, diffutils (optional) | U12a | L |
| U13 | `feat/port-python` | CPython (static), ssl, sqlite3, zlib/bz2/xz, ctypes | U9, U11, U4b | L |
| U14 | `docs/browser-prereq` → features | links; NetSurf prerequisites (SWP shm + text input, libnsfb SWP surface, image/font libs) | U11, U4a; U13 not needed | M (prereqs) |

### U1 — musl bring-up (`feat/musl-base`, L)
Kernel (one commit series, no behaviour change for existing binaries):
- auxv on the initial stack (D4 list), also for `execve`.
- `arch_prctl(ARCH_SET_FS/GET_FS)` (Linux-shaped, D3 number), FS base in
  `process_t`, saved/restored on every switch, inherited by fork, cleared by exec.
- `set_tid_address` (stores the pointer; meaningful in U9), `exit_group` =
  `exit` for now.
- Loader: arbitrary user layout + static-PIE at a random base (D4), refuse
  `PT_INTERP` and executable `PT_GNU_STACK`.
User space:
- `third_party/musl/` + `tools/port-musl.sh` + `user/musl-secos/` overlay
  (`syscall_arch.h`, `src/secos/syscall.c` table, `__set_thread_area`,
  `__restore_rt`, `posix_spawn`, `vfork`), building `build/sysroot/` (`include/`,
  `lib/libc.a`, `crt*.o`).
- The first table covers what `hello`, stdio, malloc, signals, fork/exec/wait,
  files and dirs need (≈ 60 calls).
- `tools/secos-cc`, the default note object, signing in the link step.
- `libsecos.a` + `<secos/secos.h>` for the SecOS-only calls.
Gate **MUSL-BASE** (`tools/musl_base_test.sh`): a set of musl-built signed test
programs run from the selftest: printf/scanf incl. floats, `__thread` variable,
errno, `setlocale(LC_ALL,"C.UTF-8")` + `mbstowcs`/`wcwidth`, `iconv` UTF-8↔
Latin-1, `qsort`/`bsearch`, `malloc` stress (mallocng: 100k mixed sizes),
`setjmp`, a SIGALRM-free signal handler round trip, fork/exec/wait, opendir,
static-PIE base differs between two runs (ASLR), an unsigned musl binary is
refused, an `-ENOSYS` trace line appears for a deliberately unsupported call.
Existing gates unchanged (old libc untouched).

### U2 — kernel POSIX surface (`feat/posix-kernel`, L)
All Linux-shaped, all through `copy_from/to_user`, all with `cap_check` gates
and an entry in the `kernel/bkl.c` lists:
- fd-based: `getdents64(fd)`, `fchdir`, `fstatat`/`newfstatat`, `statx`
  (subset), `openat`, `mkdirat`, `unlinkat` (+`AT_REMOVEDIR`), `renameat2`
  (`RENAME_NOREPLACE`), `linkat`, `symlinkat`, `readlinkat`, `fchmodat`,
  `fchownat`, `utimensat` (ns), `faccessat2`, `O_DIRECTORY`, `O_NOFOLLOW`,
  `O_PATH` (lookup-only fds). The kernel keeps the inode for a dir fd, so
  `*at` resolves relative to it (no path strings).
- `dup3`, `pipe2` (exists via flags), `accept4` (after U10's socket work or
  stubbed here).
- Signals: `NSIG` 64 (masks become `sigset_t` 64-bit — already 64-bit masks,
  `sig_handler[64]`), `rt_sigaction` with `SA_SIGINFO/SA_RESTART/SA_NODEFER/
  SA_RESETHAND/SA_ONSTACK`, `siginfo_t` + `ucontext_t` in the handler frame,
  `sigaltstack`, `rt_sigtimedwait`, `rt_sigpending`.
- Time: `setitimer`/`getitimer` (`ITIMER_REAL`), `alarm` (in musl via
  setitimer), `timer_create` (CLOCK_MONOTONIC/REALTIME, SIGEV_SIGNAL) optional,
  `times`, `clock_getres`, `CLOCK_PROCESS/THREAD_CPUTIME_ID`, `CLOCK_BOOTTIME`.
- `getrlimit`/`setrlimit`/`prlimit64` with real `RLIMIT_NOFILE`, `STACK`,
  `AS` (= `max_mem`), `NPROC`, `CORE`=0; `wait4` with `rusage`, `getrusage`.
- `getrandom`, `sched_setaffinity`, `madvise` (DONTNEED = zero), `msync`
  (no-op for private), `getcpu`.
Gate **POSIX2**: `/bin/posix2test` (musl-built) checks every call against
expected Linux behaviour, plus the existing PERM gate rerun with `*at` paths
(permission checks must not be bypassable through a dir fd opened before a
`chmod`).

### U3 — musl translation completeness + libc-test (`feat/musl-core`, L)
- Fill the table as U2 lands; convert structs; emulate `select/pselect`,
  `ppoll`, `epoll` (until U10), `posix_spawn` with file actions.
- `getaddrinfo`/`getnameinfo` = musl's resolver once sockets translate
  (`sockaddr_in` ↔ `secos_sockaddr`, AF_INET6 → `-EAFNOSUPPORT` so musl falls
  back cleanly).
- Locale: `C.UTF-8` default; `/usr/share/zoneinfo` (tzdata compiled by host
  `zic`, public domain) + `/etc/localtime`, so `TZ`/`localtime` work.
Gate **LIBCTEST**: musl's libc-test (MIT, `git://repo.or.cz/libc-test`, pinned)
`functional/` + `regression/` + `math/` built with `secos-cc`, run in the guest
by a driver script; the pass list is frozen into
`tools/libctest_expected.txt` and any regression fails the gate. Target:
everything except pthread/thread-dependent tests (those join in U9).

### U4a — kernel scale for big programs (`feat/kernel-scale`, L)
- Process table and kernel stack slots dynamic (keep the slot allocator but
  grow it; `MAX_PROCESSES` → 1024 with `RLIMIT_NPROC`), the 32-entry `fds[]`
  → a growable `files_t` table (default `RLIMIT_NOFILE` 1024, hard 4096). Do
  the `files_t`/`mm_t`/`sighand_t` extraction here (T2 step 1, D5), 1:1 shared,
  so U9 starts from it.
- VMA set dynamic (sorted array with binary search or a tree), mmap
  top-down with reuse, hints, `MAP_FIXED[_NOREPLACE]`, `mremap`,
  `MAP_NORESERVE`, `MAP_STACK`.
- Anonymous `MAP_SHARED` (frames shared across fork, refcounted like COW but
  never copied), `memfd_create` (an anonymous ramfs inode, `MAP_SHARED`
  mappable; also the base for GUI shared buffers, D3), a tmpfs at `/dev/shm`.
- Pipe ring 4 KiB → 64 KiB (keep `PIPE_BUF` 4096 atomicity), `F_SETPIPE_SZ`.
- Executable-memory policy (D7, Q5), default as decided, `[AUDIT] DENY EXECMEM`.
Gate **KSCALE**: 300 concurrent processes, a process with 1000 open fds,
1M mmap/munmap cycles without address-space exhaustion, 2000 VMAs, fork with
shared anonymous memory, memfd round trip, a 64 KiB pipe write is atomic in
pieces of ≤ 4096, an `mprotect(RX)` of written memory is denied (or allowed
with the flag), lockdep build clean (`make lockdep-check` subset), `-smp 4`.

### U4b — exec from the page cache (`feat/exec-pagecache`, L)
D6 in full: streaming verify, inode-backed FILE VMAs, `ETXTBSY`, verify
cache, fork without image copy, exec limit removed, page cache sized to a
fraction of RAM with LRU reclaim under memory pressure (the M50-B2 slot states
stay).
Gate **EXECBIG**: a signed 60 MB test binary execs and runs, a write to a
running binary gets `ETXTBSY`, a modified binary (written while not running)
is re-verified and refused if the signature no longer matches, the verify
cache hit shows in `/proc/secos/execcache` counters, exec of `bash` 100×
does not grow `Slab`.

### U5 — task manager (`feat/taskmgr`, M; start now)
- Kernel: `/proc/net/dev` (rx/tx bytes, packets, errors, drops per NIC; counters
  added to the NIC contract in `net/net.h`), `/proc/<pid>/status` `Threads:`
  stays 1 until U9.
- `/bin/taskmgr` per D11 on the current libc and LVGL (later rebuilt on musl
  in U7), panel launcher entry in secwm ("Task Manager"), Ctrl+Shift+Esc.
- The manifest bit `CAP_SIGNAL` added to a confined note (`note_taskmgr.S`).
Gate **TASKMGR** (extends `tools/gui_wm_drive.py`): the window opens, the
process list contains `init` and `bash`, the per-CPU graph count equals the
guest's `-smp`, a `sleep 1000` started from a terminal disappears after "End
task", ending a root process from a user session shows the refusal and the
process survives, `[AUDIT]` shows no denied cap other than the expected one.

### U6 — package delivery v2 and a bigger root (`feat/pkg2`, M; start now)
- `/bin/spkg install|remove|list|verify <file|url>` (D8), streaming verify,
  staging + rename, owned-file database, refuses a package with a path outside
  `/usr`, `/opt`, `/etc` (conf files not overwritten), `/usr/share`.
- Host side: `tools/secos-pkg` v2 (same signature format, version field 2,
  large-file streaming), `make pkgdisk PKGS=…`.
- `tools/mkroot.sh` default 8 GiB; `make grow-root`.
- Built on the current libc (moves to musl in U7).
Gate **PKG2**: install a 200 MB test package from the transfer disk, verify,
remove (no leftovers), a tampered package installs nothing, `make grow-root` →
`e2fsck -fn` clean → boot → persist test passes, kill -9 during install leaves
the old version or the new one.

### U7 — existing software on musl (`feat/musl-ports`, L)
Order (each one its own commit and gate run):
1. SecOS tools (init, login/su/sudo/passwd, usertools, netcfg, loadkeys,
   power, pkg tools, tests) — they use only POSIX + `libsecos.a`;
2. lua; 3. BusyBox (drop `busybox_compat.c` and `busybox-include/`, keep the
   applet config); 4. bash; 5. Dropbear (drop its shim); 6. secwm, sysinfo,
   swptest, sysmon, taskmgr.
Then the old `user/libc.c` is removed; `user/libsecos.c` keeps only the raw
`secos_syscall` used by the kernel ABI tests (`abi_test`, `sysfuzz`,
`testsyscall`, `uaccess_test`). **DECIDE (Q7).**
Gate: the full `tools/selftest.sh` (MUPORTS, PERM, USERS, GUI compositor,
NETSMP, M41/M42 BusyBox parts, …) green with musl builds, binary sizes and boot
time recorded in the devlog.

### U8 — ncurses + nano (`feat/port-nano`, S–M)
D12. `tools/port-ncurses.sh`, `tools/port-nano.sh` (fetch + pinned SHA-256 +
`secos-cc`), delivered as a package (U6) or embedded until U6 lands.
Gate **NANO**: over a pty (`tools/nano_drive.py`, the `ssh_winch.py` approach
over Dropbear) and in a secwm terminal (`gui_wm_drive.py`): open a file, type
UTF-8 text (`à è`), search, cut/paste a line, resize the window (SIGWINCH
redraw), save, exit; the file's bytes are checked from the shell. Also in the
fb console with `TERM=linux`.

### U9 — threads (`feat/threads`, XL)
D5 in full, in steps that each pass the selftest:
1. thread groups: `clone` with the musl flag set, tgid, `gettid`, `exit` vs
   `exit_group`, `CLONE_CHILD_CLEARTID`, `/proc/<pid>/task`;
2. futex (WAIT/WAKE/REQUEUE/CMP_REQUEUE/BITSET, timeouts, private + shared);
3. signals per thread (mask, pending, routing, `tgkill`, group stop/continue,
   `execve`/`fork` from a thread);
4. musl: enable pthreads in the overlay (`pthread_create` → SecOS clone, cancel
   via SIGCANCEL), rebuild the sysroot;
5. lockdep run + `smptest` additions; `Threads:` real in procfs and taskmgr.
Gate **THREADS**: libc-test pthread tests join the LIBCTEST pass list;
`/bin/thrstress` (N threads × mutex/condvar/rwlock/barrier/semaphore,
`pthread_cancel`, `pthread_join` of 10k short threads, a thread `execve`, kill
of a multi-threaded process, SIGINT to a group) on `-smp 1/2/4`, with
`-DSECOS_LOCKDEP`; no `Slab` growth after the run.
Risk: this changes the scheduler's object model a few weeks after the BKL
removal. Keep `nothreads` (boot word refusing `clone`) as the fallback.

### U10 — network userland (`feat/net-posix`, L)
- Sockets Linux-shaped: `AF_INET` `SOCK_STREAM/DGRAM` with `SOCK_NONBLOCK`/
  `SOCK_CLOEXEC`, `setsockopt/getsockopt` (`SO_REUSEADDR`, `SO_KEEPALIVE`,
  `SO_RCVTIMEO/SNDTIMEO`, `SO_ERROR`, `SO_RCVBUF/SNDBUF` read-only,
  `TCP_NODELAY`), `shutdown`, `sendmsg/recvmsg` (no ancillary at first),
  `MSG_PEEK/DONTWAIT/NOSIGNAL`, `accept4`; raise `MAX_SOCK`/`TCP_MAX_CONN` to
  1024.
- `AF_UNIX` stream + datagram, `socketpair`, filesystem paths (permission-checked
  like files) and abstract names; `SCM_RIGHTS` later.
- `epoll` (`create1/ctl/pwait`), built on the existing poll wakeups;
  `eventfd`; `timerfd` (libuv and Python's asyncio use them; libuv can fall back
  to poll, see U12b).
- `/etc/resolv.conf` written by `ifup`, `/etc/hosts`, `/etc/services`,
  `/etc/protocols`; BusyBox `ntpd` + `hwclock` enabled; init runs the time sync
  (Q13); CA bundle package.
Gate **NETPOSIX**: a test suite against the host (QEMU user-net, host services
started by the driver): non-blocking connect + poll + `SO_ERROR`, a refused port,
DNS through musl's resolver against a stub DNS server on the host, AF_UNIX echo
with permissions, `epoll` with 500 fds; NETSMP still green.

### U11 — OpenSSL, curl, wget (`feat/port-tls`, M–L)
- OpenSSL 3.5 LTS (Apache-2.0; 3.0 LTS reached end of life in Sept 2026):
  `no-shared no-dso no-engine no-threads` first, `enable-ktls` off, `openssl`
  CLI included. Rebuilt with threads after U9.
- curl 8.x (curl licence) with OpenSSL, zlib, no threaded resolver until U9.
- GNU wget 1.25 (GPL-3.0+, with its OpenSSL linking exception), `--with-ssl=openssl`.
- Manifests: `CAP_NET` (explicit) for curl/wget/openssl.
- zlib (zlib licence) ported as a shared dependency (also for Python, links,
  NetSurf, git).
Gate **TLS** (offline): the driver starts on the host an HTTPS server with a test
CA (installed in the guest only for the test) and serves files over 1 MB.
`curl -sSf https://10.0.2.2:PORT/f` and `wget` downloads match the SHA-256, a
certificate from an unknown CA is rejected, an expired certificate is rejected,
a guest clock moved 2 years ahead makes validation fail,
`openssl s_client`/`speed sha256` run, `wget -r` of a small tree works.

### U12a — native toolchain (`feat/port-toolchain`, XL)
- Host: build a **cross** toolchain for the sysroot (binutils 2.45, GCC 15.x,
  C and C++, `--disable-shared --enable-static --disable-plugin
  --disable-libsanitizer --enable-threads=single` at first,
  `--with-sysroot`), with the SecOS specs (static-PIE default, note object,
  sign hook, `-D__secos__`). It replaces `tools/secos-cc` for all ports.
- Canadian cross: the same toolchain built **to run on SecOS** (static,
  signed): `gcc`, `g++`, `cpp`, `cc1`, `cc1plus`, `collect2`, `as`, `ld.bfd`, `ar`,
  `nm`, `objdump`, `readelf`, `strip`, plus GNU make 4.4 and the SDK
  (musl headers + `libc.a`, `libstdc++.a`, `libgcc.a`, crt objects) as one
  package `secos-sdk` (≈ 300–450 MB installed).
- Signing on the device (D9 / Q4): the keyring kernel work, the signer service,
  `ld` wrapper; `secos-sign` written in C with an Ed25519 *sign* implementation
  (the kernel has only verify; e.g. the ref10 code already used for verify,
  extended, or Monocypher, CC0/BSD).
- Disk/RAM: 8 GiB root (U6), guest ≥ 2 GiB RAM for the gate (`cc1plus` on a
  heavy C++ file uses 300–800 MB). Compiling gcc itself on SecOS is out of scope
  (needs ~8 GB disk, hours, and perl).
Gate **TOOLCHAIN** (in the guest): compile + sign + run `hello.c`, `hello.cpp`
(iostream, `std::string`, exceptions, `std::sort`), a C program with
`-O2 -fstack-protector-strong`; `make -j2` builds lua 5.4 from source on SecOS
and its test script output matches the host-built lua; the result runs under
the local key and is refused after `revoke` of that key; a binary whose
manifest asks for `DRIVER` cannot be signed locally.

### U12b — cmake, ninja (`feat/port-cmake`, L)
- Rebuild the toolchain with `--enable-threads=posix` (needs U9) so
  `std::thread` works.
- cmake 4.x needs libuv (threads + epoll, or libuv's poll backend), curl
  (from U11) or its bundled copy, C++17. ninja 1.12 (or samurai, ISC, C only).
  pkgconf.
Gate: configure + build a CMake project (e.g. a small C++ library with tests)
on SecOS with both the Makefile and Ninja generators; `ctest` passes.

### U12c — developer tools (`feat/port-devtools`, L, optional)
perl 5 (needed by autoconf/automake and by OpenSSL's own build), m4, autoconf,
automake, libtool (static), bison, flex, patch, diffutils, git (no `NO_PTHREADS`
after U9; HTTPS via curl). Gate: `./configure && make` of a GNU package
(e.g. GNU hello) on SecOS; `git clone https://…` from the host server.

### U13 — Python (`feat/port-python`, L)
- CPython 3.13 or 3.14 (PSF), static, built-in extension modules
  (`Modules/Setup.local`), `_ssl` (OpenSSL), `sqlite3` (public domain,
  `SQLITE_THREADSAFE=1` after U9), `zlib`, `_bz2` (bzip2), `_lzma` (xz), `_ctypes`
  with libffi 3.4 static trampolines (needs no exec memory, D7), `_decimal`
  (bundled mpdecimal), `readline` via libedit or none (GNU readline would turn
  python GPL).
- `subprocess` (fork+exec or posix_spawn, closes fds via `/proc/self/fd`),
  `multiprocessing` (needs `/dev/shm` + `MAP_SHARED` + `sem_open`: U4a),
  `asyncio` (epoll: U10).
- stdlib as a package (≈ 50–120 MB with `.pyc`), `pip` allowed or not:
  **DECIDE (Q12)** (pip runs code from the network as the user; SecOS's signature
  never covers it).
Gate **PYTHON**: `python3 -c` basics, a chosen regrtest subset (`test_os`,
`test_threading`, `test_subprocess`, `test_socket` (AF_INET/AF_UNIX parts),
`test_ssl` against the host CA server, `test_json`, `test_re`, `test_sqlite3`,
`test_ctypes`), with the pass list frozen like LIBCTEST.

### U14 — browser prerequisites (`docs/browser-prereq`, plan + small features)
links (text mode) first: needs OpenSSL (U11), zlib, a terminal (works in
console, secwm terminals, SSH). Mostly a port (S–M).
NetSurf (framebuffer frontend) needs, besides OpenSSL/curl:
- **SWP shared buffers:** pushing 2.9 MB frames through a 4 KiB pipe is too
  slow for scrolling. Add a `memfd`-backed shared buffer passed at client
  creation (`SWP_REQ_SHM`, the buffer mapped `MAP_SHARED` by both sides; the
  compositor still copies out, never trusts the contents' size), using U4a's
  `memfd_create`. This replaces the `TASKS.md` 92–94 reservation (D3).
- **SWP text input:** keys as Unicode code points + keysyms (SWP_EV_KEY today
  carries key codes + modifiers), key repeat, clipboard later.
- **libnsfb SWP surface** (`surface/swp.c`), next to its linux/sdl/vnc ones.
- Libraries: libpng, libjpeg-turbo, libwebp (optional), freetype + a font
  package (DejaVu), libcss/libdom/libhubbub/libparserutils/libwapcaplet/
  libnsgif/libnsbmp/libnsutils/libnspsl/libutf8proc (all MIT-family), host-side
  nsgenbind; iconv from musl.
- Window size above 1024×720 (`SWP_MAX_W/H`) with the shm path.
- Threads are not needed by NetSurf.

---

## 4. Parallelism and merge order

```
now ── U5 (taskmgr) ──────────────────────────────┐(rebuild in U7)
now ── U6 (pkg2, bigger root) ────────────────────┼──────────────┐
now ── U1 ─┬─ U2 ──┬─ U3 ─┬─ U7 ──────────────────┤              │
           │       │      ├─ U8 (nano)            │              │
           │       │      └─ U10 ── U11 ─┬────────┼── U12b ── U13│
           └─ U4a ─┴─ U9 ────────────────┤        │              │
                   └─ U4b ───────────────┴── U12a ┴── U12c       │
                                          U14 (after U11, U4a) ──┘
```
- **Now, in parallel:** U1 (kernel part small), U5 and U6 (user space, plus
  `/proc/net/dev`). The nano and ncurses recipes can start against the host's
  `musl-gcc` (`apt-get download musl-tools` into `~/.local`, no root, as for
  exFAT) while U1 runs.
- U2 and U4a both edit `kernel/syscall.c`, `process.{c,h}`, `kiface.c`: merge U2
  first, or split the work by file. U4a's `files_t` extraction touches every
  `c->fds[...]` user: one agent, one series, rebased onto Phase 5's IRQ-fd
  work.
- U9 starts after U4a (it needs `mm_t/files_t/sighand_t`). U10 and U9 can run
  in parallel (different files, except `kiface.c` poll: U10 first).
- U7 can start per program as soon as U3 covers its calls; BusyBox last but
  one (largest shim), secwm last (GUI gate).
- Phase 5 conflicts: `SYS_UDRV` 92 is theirs. Kernel-heavy U2/U4a/U9 should
  merge after Phase 5's M57 (Driver Space v2 core) or be rebased onto it;
  **DECIDE (Q10)**.

## 5. Gates summary (new selftest sections)
| Gate | Milestone | Driver |
|---|---|---|
| MUSL-BASE | U1 | `tools/musl_base_test.sh` |
| POSIX2 | U2 | `/bin/posix2test` + PERM rerun |
| LIBCTEST | U3, U9 | `tools/libctest.sh`, frozen pass list |
| KSCALE | U4a | `/bin/kscale` + lockdep subset |
| EXECBIG | U4b | `tools/execbig_test.sh` |
| TASKMGR | U5 | `tools/gui_wm_drive.py --taskmgr` |
| PKG2 | U6 | `tools/pkg2_test.sh` (+ grow-root) |
| full selftest on musl | U7 | `tools/selftest.sh` |
| NANO | U8 | `tools/nano_drive.py` |
| THREADS | U9 | `/bin/thrstress`, `-smp 1/2/4`, lockdep |
| NETPOSIX | U10 | `tools/netposix_drive.py` + host stubs |
| TLS | U11 | `tools/tls_drive.py` (host HTTPS + test CA) |
| TOOLCHAIN | U12a | `tools/toolchain_test.sh` (2 GiB guest) |
| CMAKE / DEVTOOLS | U12b/c | in-guest builds |
| PYTHON | U13 | regrtest subset, frozen list |

Long gates (TOOLCHAIN, PYTHON, LIBCTEST) get their own `make` targets and stay
out of the default `selftest.sh` run, like `make locktests`.

## 6. Risks
- **Kernel work dominates.** U2, U4a/b and U9 together are 4–6 months of
  agent-time and touch the core just reworked by M48–M52. Mitigation: every
  step behind its gate, lockdep runs, `nothreads`-style fallbacks.
- **Translation semantics.** A converted struct or flag that differs subtly
  from Linux becomes a libc bug in every program. Mitigation: LIBCTEST, the
  `-ENOSYS` trace, Linux-shaped new calls (identity translation).
- **Performance.** `int 0x80`, polled virtio-blk, one ext4 lock, a 512 KiB page
  cache and SHA-256 per exec make compiling slow. U4b fixes the cache and the
  hashing; a `syscall`-instruction fast path for the native ABI and a
  finer-grained ext4 lock are follow-ups. Expect compile times several times
  Linux's on the same VM.
- **Threads.** Lost wake-ups (futex vs the re-run model), signal routing, exec
  and fork from threads, TLB shootdown with many threads on many CPUs. Large
  test surface; CPython and cmake will find the rest.
- **Address-space change.** Moving to standard layouts and static-PIE changes
  the loader that every program goes through. Keep accepting `user.ld`
  binaries until U7 is done.
- **Security drift.** The userland TCB grows by orders of magnitude (OpenSSL,
  gcc, Python). The kernel boundary does not move, but: compilers and
  interpreters run code nobody signed (D7, D9); a local signing key on the
  device is a new high-value secret (D9 S2: root-only, audit, revocable, capped);
  pip/CA bundle/NTP add network trust decisions (Q12, Q13).
- **Disk and RAM.** SDK ≈ 400 MB, Python ≈ 100 MB, build trees GBs. The 1 GiB
  root is too small; `grow-root` on existing images must be proven against the
  SecOS ext4 driver. ext4 multi-instance stays a gap.
- **Licences.** More GPL programs in the image (gcc, binutils, make, nano, wget):
  same policy as bash/BusyBox (fetched, never in-tree, written offer in
  `docs/THIRD_PARTY.md`). libgcc/libstdc++ use the GCC runtime exception, so
  linking SecOS programs with them is fine. OpenSSL 3 is Apache-2.0
  (compatible with GPL-3, not GPL-2-only).
- **Upstream versions.** OpenSSL 3.0 is already end-of-life; pin current LTS
  versions when each milestone starts and record SHA-256s.
- **Phase 5 overlap** in `syscall.c`, the fd table, poll and teardown (§1.8).

## 7. Decisions for the user
- **Q1.** libc: musl with user-space translation (B1, recommended), keep growing
  the home-made libc (A), or a kernel Linux personality (B2)?
- **Q2.** Numbers for new Linux-shaped syscalls: `512 + Linux number`
  (recommended), next free numbers, or Linux numbers where free?
- **Q3.** Toolchain triple: `x86_64-linux-musl` + `__secos__` (recommended:
  every configure script works) or `x86_64-secos-musl` (patch every
  `config.sub`)?
- **Q4.** Signing programs compiled on SecOS: S2 local machine key with a
  capability ceiling and a password-gated signer (recommended), S1 developer
  mode for unsigned confined binaries, S3 per-user keys, or S0 none?
- **Q5.** Executable anonymous memory (`mprotect` RW→RX): forbid by default with a
  signed `EXECMEM` opt-in (recommended), or keep allowing it?
- **Q6.** Room for big software: 8 GiB root + `grow-root` + packages v2
  (recommended), or invest in multi-instance ext4 for a separate SDK volume?
- **Q7.** After the migration, delete `user/libc.c` and keep only the raw
  `libsecos` layer for ABI tests (recommended)?
- **Q8.** nano: build it on musl right after U3's subset (recommended), or a
  quicker throwaway port on the current libc now?
- **Q9.** Task manager "End task" on other users' processes: refuse (recommended
  for now), or plan a GUI elevation prompt?
- **Q10.** Run this phase alongside Phase 5 (U1, U5, U6 now; kernel-heavy U2/U4/U9
  merged after Phase 5's M57) or after Phase 5 finishes?
- **Q11.** Versions to pin: OpenSSL 3.5 LTS, GCC 15, binutils 2.45, CPython
  3.13/3.14, cmake 4.x, nano 8.x — any preference (e.g. an older GCC for size)?
- **Q12.** Python: allow `pip` (network code that SecOS never signs), and full
  stdlib or a reduced one?
- **Q13.** Time sync: on by default at boot (which NTP server), or opt-in?
- **Q14.** Should the boot-critical set (init, login, bash, BusyBox, Dropbear,
  secwm) stay embedded in the kernel image, with everything else moving to
  packages (recommended)?
