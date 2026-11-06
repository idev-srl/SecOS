# M44 — SMAP by default + no low identity map

Phase 4 hardening (branch `feat/smap-noident`). Two changes that close the
same class of bug from two sides: the kernel touching memory it should not
reach through a user-controlled pointer.

1. **SMAP is on by default** whenever CPUID reports it.
2. **The low identity map is gone** from every user address space, and the
   kernel retires it from its own page tables after early boot.

On the way, a third bug turned up: the **stack canary was read from
`%gs:0x28`**, which worked only because of the identity map.

## 1. SMAP by default

- `cpu_enable_mitigations()` (`kernel/kernel.c`) sets `CR4.SMAP` when
  `CPUID.7:EBX[20]` is present. The opt-out is `-DSECOS_NO_SMAP` (it was
  opt-in `-DSECOS_SMAP` since M36). The BSP and every AP do this.
- Boot line: `[SMAP] enabled: kernel user access only via uaccess (stac/clac)`,
  or `[SMAP] not supported by this CPU (fallback: no SMAP)`, or
  `[SMAP] supported but disabled (SECOS_NO_SMAP)`.
- **AC is cleared on every kernel entry.** Interrupt, exception and `int 0x80`
  delivery do not clear `RFLAGS.AC`. Without a fix, ring 3 could `popf` AC=1
  and enter the kernel with SMAP switched off for the whole handler. Every
  entry stub (`isr_common`, `isr_timer`, keyboard, mouse, xHCI, NVMe, net,
  `syscall_entry`) now runs `SMAP_CLAC`. This is `clac`, guarded by
  `g_smap_enabled`, because `clac` is `#UD` on a CPU without SMAP. `isr_stub`
  is only an `iretq` and runs no C. There is no `SYSCALL`/`FMASK` path: the
  only syscall gate is `int 0x80`. On the way back, `iretq` restores the
  interrupted AC. So an interrupt that lands inside a uaccess copy resumes it
  with AC=1, which is correct.
- **A stray access panics clearly.** `exception_handler` (`kernel/panic.c`)
  checks for a SMAP violation first: a ring-0 data `#PF` (P=1, not an
  instruction fetch) on an address below `USER_ADDR_MAX`, with AC=0 in the
  saved RFLAGS. It logs
  `[SMAP] violation: ring-0 read|write of user address cr2=… rip=…` and calls
  `kernel_panic`. It never retries through demand paging or COW. A stray
  access to a page that is not yet present is first demand-paged by the old
  path, then faults P=1 on the retry, and so lands here as well.
- The only sanctioned accesses are the uaccess primitives in
  `mm/user_copy.c` (`copy_from_user`, `copy_to_user`, `strncpy_from_user`,
  bracketed by `stac`/`clac`) and the fault-tolerant asm in
  `arch/x86/uaccess.asm`.

### User-memory audit

I checked every kernel path that touches user memory. They already go through
the uaccess helpers or the physmap, thanks to the M3 hardening and the later
`fix/uaccess-fault` work. No path needed converting for SMAP. The audit was
done twice:

- **By reading the code.** Every `ksys_*` / `syscall_dispatch` /
  `syscall_trap` argument that is a user pointer, plus the subsystems below.
- **By running everything with SMAP on.** A full `tools/selftest.sh` run used
  a QEMU wrapper that defaults to `-cpu max`, so SMAP was on in every gate
  that does not pick its own CPU. That run produced 0 `[SMAP] violation`
  lines. See *Tests*.

| path | how it reaches user memory |
|---|---|
| demand-paging fault fill (`mm/vma.c vma_fault_in`), COW copy (`vmm_cow_fault`) | physmap alias of the frame |
| exec argv/envp build (`kernel/process.c`, in-place `execve`) | physmap of the materialized stack page |
| signal frame setup / `sigreturn` (`kernel/signal.c`) | `copy_to_user` / `copy_from_user` |
| fork (`vmm_fork_space`, `process_fork`) | page tables + physmap only |
| read/write on files, pipes, the console TTY, ptys, evdev (`ksys_read`/`ksys_write`) | bounce buffers (`uio_get`) + `copy_*_user` |
| sockets (`SYS_SEND/RECV/SENDTO/RECVFROM`, `ksys_sock_*`) | `netbuf` bounce + `copy_*_user` |
| fbdev read/write/ioctl, evdev ioctls (`drivers/fbdev.c`, `drivers/input.c`) | `copy_*_user` |
| getdents, getcwd, chdir, stat/lstat/fstat, readlink, mount strings | `kmalloc` buffer / `strncpy_from_user` + `copy_to_user` |
| ioctl handlers (`ksys_ioctl`, `kernel/tty.c`, `kernel/pty.c`, `kernel/kiface.c`) | `copy_*_user` |
| IPC `msg_send`/`msg_recv` (`kernel/ipc.c`) | validated + bounce-buffered |
| `SYS_DRIVER`, `SYS_NETCTL`, cred syscalls | `copy_*_user` |
| sigprocmask oldset, pipe2 fds, spawn argv array + strings | `copy_*_user` / `strncpy_from_user` |

## 2. No low identity map

Before M44, the kernel PML4[0] mapped physical 0–512 MiB at virt == phys
(2 MiB pages, NX except the first 2 MiB). `vmm_space_create_user()` copied
those entries, as supervisor pages, into every user CR3's private PML4[0]
PDPT. User space could not touch them. Any **ring-0** access through a low
user-supplied pointer, however, reached real physical memory. The
`USER_ADDR_MIN` floor in `user_range_valid` was the only thing stopping it.

### User address spaces

`vmm_space_create_user()` now shares only the kernel **high half**
(PML4[256..511]). PML4[0] is a private PDPT that starts empty, and
PML4[1..255] are left empty. The lower half of a user CR3 holds user pages
and nothing else. `vmm_space_low_kernel_entries()` counts any present
lower-half entry outside the user window. At boot, a fresh user space is
created, audited and destroyed:
`[IDMAP] user CR3 audit: no low kernel mappings (OK)`.

### The kernel's own tables: the identity map is gone there too

`vmm_drop_low_identity()` runs at the start of phase 2, right after the W^X
audit (which still inspects the identity map while it exists). It clears the
kernel PML4[0] and reloads CR3. The identity entries are non-global, so the
reload flushes them. Log line:
`[IDMAP] low identity map removed from the kernel page tables (…)`.

The identity map is still used in two places:

- **Early boot (phase 1).** The `boot.asm` / UEFI-loader tables, the PMM
  parsing the MB2/UEFI memory map, `vmm_init`, and the physmap build all run
  before the physmap exists and need the identity map.
- **The AP trampoline.** An AP starts in real mode at phys `0x8000`. It must
  execute there for a few instructions after enabling paging, so the
  identity tables are kept, but only behind a dedicated **trampoline CR3**:
  the kernel high half plus the boot identity PDPT. That PML4 is allocated
  below 4 GiB, because the trampoline loads CR3 with a 32-bit `mov`.
  `ap_entry()` switches to the kernel CR3 as its first statement. Neither the
  kernel CR3 nor any user CR3 maps low memory, both at boot (with
  `SECOS_SMP_AT_BOOT`) and for a later shell `smp`.

### Identity-map users converted

| user | before | now |
|---|---|---|
| **stack canary** (every `-fstack-protector-strong` function) | `%gs:0x28` with GS base 0, i.e. linear `0x28` read through the identity map (the real-mode IVT) | `-mstack-protector-guard=global`: the `__stack_chk_guard` variable |
| VGA text buffer (`lib/terminal.c`) | `0xB8000` | `phys_to_virt(0xB8000)` once the physmap is up (`terminal_relocate_vga()`; also on the panic path's `terminal_initialize()`) |
| MB2 framebuffer tag parse (`fb_init`) | MB2 info pointer as virt | physmap |
| low framebuffer (< 16 MiB) (`fb_finalize_mapping`, `fb_console_init`, the boot pixel test) | `virt_addr = 0` → used `addr` | always the physmap VA |
| ACPI tables (`arch/x86/acpi.c pv()`) | identity below 512 MiB, physmap above | always the physmap, extended explicitly to cover each table (+1 MiB): this is what made M28's "physmap may not cover ACPI on UEFI" case work, and it now applies to all tables, including late users such as the FADT reset register at reboot |
| AP trampoline copy + parameter block (`kernel/smp.c lowp()`) | identity | physmap |
| AP CR3 | `read_cr3()` (the kernel CR3 with identity) | `vmm_ap_trampoline_cr3()` |
| `kvirt_to_phys` (all DMA drivers) | anything not image/physmap → returned `v` (identity semantics); the M2/M5 stack region fell into the physmap branch and returned garbage | image / physmap (bounded to its PML4 slot) / **page-table walk** |
| `vmm_translate*` | 4 KiB-only walk (misread a huge-page PDE as a PT) | full walk honouring 1 GiB / 2 MiB pages |

Already correct, and only checked: virtio-blk, AHCI, NVMe, xHCI, the USB
class drivers, e1000/e1000e/vmxnet3/igc and ath9k all DMA from static
kernel-image buffers, which `kvirt_to_phys` handles in its image branch. MMIO
is reached via `vmm_extend_physmap` + `phys_to_virt`, the LAPIC/IOAPIC via the
physmap, and the heap, page cache and PMM bitmap via the physmap or image VAs.

### Why the stack canary mattered

With `-mcmodel=kernel`, GCC's default stack-protector guard is the TLS slot
`%gs:0x28`. SecOS never sets a GS base, so the guard was the eight bytes at
linear address 0x28. That is the real-mode IVT, a fixed and guessable value,
and it was readable only because the identity map was present in every CR3,
the user ones included. The `__stack_chk_guard` that M36 defined was never
used. Dropping the identity map made the first protected function fault at
`cr2=0x28`, and that is how this bug was found. The kernel now has 0 `%gs:`
references; the selftest checks this.

## Tests

- `user/uaccess_test.c` gains `[M44]` cases:
  - `noident-mmap-hint-low-ignored`
  - `noident-{read,write}-{physmap,kimage}-ptr`: kernel-half pointers →
    `-EFAULT` in both directions
  - `noident-ring3-load-0x100000-sigsegv`: now a not-present fault (`err=4`;
    before, `err=5`, a protection fault on a present supervisor page)
  - `noident-sigframe-low-rsp-killed`: the process has a SIGCHLD handler and
    an rsp in the old identity window. The kernel refuses to build the frame
    and the process dies *by* SIGCHLD, which proves no frame was written.
  - `smap-entry-with-ac-set`: a syscall entered with AC=1 still works.

  The UACCESS gate now runs on `-cpu max`. 48/48 cases pass, ABI 36/36.
- New selftest checks:
  - the default image on `-cpu max`: SMAP+SMEP on, user CR3 audit OK, reaches
    init, no exception or violation
  - the Haswell boot (SMEP, no SMAP) shows the fallback line and the
    `[IDMAP]` lines
  - `objdump` finds no `%gs:0x28` reads
  - a `SMAP_VIOLATION_DEMO` image reads a user page directly from ring 0 and
    must log `[SMAP] violation … cr2=0x0000000100000000` and `[PANIC] SMAP
    violation`. On a CPU without SMAP the probe is skipped and the kernel
    keeps running.
- Smoke: `tools/smoke.sh --mb2` and `--uefi` pass on the default CPU (qemu64:
  no SMAP, fallback) and on `-cpu max` (SMAP on).
- SMP: `SECOS_SMP_AT_BOOT` with `-smp 4 -cpu max` brings all 4 CPUs online,
  and user tasks run on APs. The late shell `smp` with `-smp 2` works too.
- Soak: `tools/soak.sh --smp 4 --disk --fuzz` (SECOS_SMP_AT_BOOT, `-cpu max`,
  ext4 virtio disk, the syscall fuzzer) passed 200/200 commands. No kernel
  fault, and all sentinels were present.
- Disk drivers: the M10 (virtio fat32/ext2/ext4), M21 (AHCI), M22 (NVMe, USB
  xHCI+HID+MSC) and M23 sections on `-cpu max` gave 130 PASS / 0 FAIL.
- Full `tools/selftest.sh` runs:
  - SMAP-only tree, every QEMU defaulting to `-cpu max`: 882 pass / 8 fail,
    **0 `[SMAP] violation`**.
  - Final tree on the default CPU: 904 / 9.
  - Final tree on `-cpu max`: 903 / 10.
  Every failure in these three runs comes from either the test harness or
  the environment; see *Selftest notes*. I re-ran every gate that failed (UEFI
  poweroff, secure boot, the SMAP violation demo, UACCESS + ABI, M40 UEFI
  fb0, crypt KAT) in a correct environment (`PYENV_VERSION=3.11.8`) on the
  final tree: **108/108 PASS on the default CPU and 108/108 PASS on
  `-cpu max`**.
- `noident-*` / SMAP cases: all pass on both CPUs. The `[SMAP] violation`
  line appears only in the deliberate `SMAP_VIOLATION_DEMO` log.

### Selftest notes

- **Unsigned kernel → UEFI gates fail.** This affects the UEFI poweroff,
  secure boot, M40 UEFI fb0 and crypt KAT build gates. When the selftest
  runs under a private `/tmp` (`unshare -rm`, as the parallel agents do),
  pyenv resolves `python3` to an interpreter without `cryptography`.
  `tools/secos-sign` is then skipped: the Makefile prints a warning and
  carries on. The secure-boot loader therefore refuses `kernel.elf`, and
  OVMF drops to its shell. `main` does the same in that environment.
  Exporting `PYENV_VERSION=3.11.8` fixes it.
- **The M10 gates showed a "program ran twice" `#GP`.** This is a real
  pre-existing race that also exists on `main` (2/24 boots under host load).
  It is fixed in this branch: `kernel/shell.c` now holds a new process until
  the shell has wired it up, and never resurrects one that already ran. With
  the fix it happened 0 times in 36 loaded boots.
- `uaccess spawn-wild-path` was a log interleave: a `[SCHED]` trace landed
  between the case name and `OK`. Each result line is now one `write()`.
- `M44 no SMAP CPU` failed in the `-cpu max` run only, because the wrapper
  turned "no -cpu" into `-cpu max`. The gate now pins `-cpu qemu64`.

## Performance

- `SMAP_CLAC` adds a compare and a predicted branch, plus `clac`, to each
  kernel entry.
- `stac`/`clac` was already present around each uaccess copy; M44 only turns
  it on.
- Dropping the identity map has no runtime cost: the physmap was already the
  main path.
- In the selftest, the bash, BusyBox, SSH and GUI gates showed no measurable
  change in wall time.

## Open issues / follow-ups

- `__stack_chk_guard` is a fixed constant. It should be randomized at boot
  (RDRAND/TSC) from a function without a stack protector. That fits best
  with the KASLR work.
- The first 2 MiB of the identity map is still W+X, now only inside the
  transient AP-trampoline CR3. It could be split so that only the trampoline
  page is executable.
- `boot/boot.asm` and the UEFI loader still build their own identity maps for
  early boot. That is unavoidable before the physmap exists, and those tables
  are abandoned at `vmm_init`.
- The MB2 info block is parsed for the framebuffer at the end of phase 2,
  long after PMM init. This happened before M44 as well (through the identity
  map). It relies on the block's frames not having been reused.
