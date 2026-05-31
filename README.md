[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE.md)
[![Build](https://img.shields.io/badge/build-passing-brightgreen)]()
[![Made by iDev](https://img.shields.io/badge/made%20by-iDev%20Srl-blue)](https://www.idev-srl.com)

# SECoS — Secure OS Kernel (x86-64)

A minimal secure kernel written in C/ASM, boots via UEFI (primary path) or GRUB Multiboot2 (legacy path), targeting x86-64 long mode.

## Current Status — M8 (Preemptive Multitasking, core done)

**Milestone M8 (Preemptive Multitasking + Process Lifecycle)**: core complete (verified at N=2). See [`docs/devlog/M8.md`](docs/devlog/M8.md).

- Timer-driven preemptive scheduler (trapframe `isr_timer`, quantum, ring-3-only preemption) + kernel idle task
- `SYS_EXIT` → zombie + reaping; `vmm_space_destroy` fixed to free the private `PML4[0]` PDPT; no PMM leak across rounds
- Build the demo: `make CFLAGS_EXTRA=-DM8_SCHED_DEMO=1`; verify with `tools/selftest.sh`
- ⚠️ Known issue: N≥3 spawn corruption (a frame-aliasing/identity-map bug, not the scheduler) — first M9 item; demo uses N=2



**Milestone M7 (Ring-3 Entry + Cooperative Scheduling)**: complete. Tag `M7_STABLE`. See [`docs/devlog/M7.md`](docs/devlog/M7.md).

- `arch_enter_user_mode()` performs the ring-0 → ring-3 transition into a user process built from a synthetic ELF
- `SYS_YIELD` (syscall 0) drives `sched_yield_from_syscall()`, which saves the caller trapframe, picks the next `READY` process and resumes it via `iretq`
- Two ring-3 processes alternate cooperatively via `SYS_YIELD` (verified: hundreds of thousands of clean `[SCHED] switch` per run, both directions, no CPU exception, on MB2 + UEFI)
- Demo is gated behind `M7_RING3_DEMO` (off by default so normal boot reaches the shell): `make CFLAGS_EXTRA=-DM7_RING3_DEMO=1`
- Non-interactive verification: `tools/selftest.sh` (asserts M4 12/12 + alternating ring-3 switches + no `[EXC]`)
- Four bugs were fixed to get here (premature timer preemption, `EFLAGS.NT` → `iretq` #GP, supervisor `PML4[0]` + shared user PDPT, 2× stride in ELF copy) — see the devlog

**Milestone M6 (Minimal Context Switch)**: complete. See [`docs/devlog/M6.md`](docs/devlog/M6.md).

- `trapframe_t* tf` added to the PCB; `process_create_from_elf` builds the initial iret frame (RIP=entry, RSP=stack_top, RFLAGS=0x202)
- `arch_iret_to_tf` / `arch_switch_to_process` restore all 15 GPRs and `iretq` into the target after `vmm_switch_space`
- Syscall and exception handlers persist the live trapframe into `current->tf`

**Milestone M5 (Trapframe Syscall Entry + Kernel Stack Hardening)**: complete. See [`docs/devlog/M5.md`](docs/devlog/M5.md).

- Unified `INT 0x80` entry into the canonical `trapframe_t` layout (matches `isr_common` GPR save/restore); C `syscall_handler(trapframe_t*)`
- Replaced PID-based kernel-stack slots with a bounded slot index

**Milestone M4 (Stabilization & Isolation Test Suite)**: complete. Tag `M4_STABLE`. See [`docs/devlog/M4.md`](docs/devlog/M4.md).

- 12-case in-kernel selftest suite for `user_range_valid` / `copy_from/to_user` (all PASS)
- `vmm_map_in_space()` supervisor enforcement aligned with `vmm_map()` (R1 closed)
- Smoke tests: MB2 (exit 124 PASS) + UEFI (exit 124 PASS)
- Selftest controlled by `M4_SELFTEST_ENABLE` (default: 1)

**Milestone M3 (User/Kernel Isolation)**: complete. Tag `M3_ISOLATION_BASE`. See [`docs/devlog/M3.md`](docs/devlog/M3.md).

- `user_range_valid`, `copy_from_user`, `copy_to_user` — safe boundary-crossing primitives
- `vmm_map()` supervisor enforcement, `vmm_harden_user_space()` fixed, PML4[256+] assert
- All syscall user pointers validated (SYS_OPEN/READ/WRITE/DRIVER)

**Milestone M2 (Stack and Exception Hardening)**: complete. Tag `M2_STABLE`. See [`docs/devlog/M2.md`](docs/devlog/M2.md).

- Dedicated virtual stack region at `0xFFFFFF8000000000` (PML4[511])
- Kernel main stack: 16 KB usable + guard_lo / guard_hi (not-present PTEs)
- IST1/2/3 stacks: 8 KB each, full guard pages, virtual addresses in TSS
- Debugcon boot markers: `SECoS build <TS> git:<HASH>` + `[M2] Stack switch ok`
- Deterministic smoke test: `tools/smoke.sh` (exit 124 = PASS for both MB2 and UEFI)

**M1 (Memory Model Hardening)**: complete. Tag `M1_STABLE`. See [`docs/devlog/M1.md`](docs/devlog/M1.md).

### Running the Isolation Selftest

```bash
# Build and run (selftest is enabled by default, M4_SELFTEST_ENABLE=1):
make iso && tools/smoke.sh --mb2 --timeout 20 --log /tmp/secos_mb2.log
# Check debugcon log for [M4][SELFTEST] lines (expect: 12/12 PASS)

# Disable selftest at build time:
make iso CFLAGS_EXTRA=-DM4_SELFTEST_ENABLE=0
```

## Features

- ✅ Long Mode (64-bit) boot
- ✅ Multiboot 1/2 support (GRUB)
- ✅ **UEFI boot support** ([see UEFI_IMPLEMENTATION.md](UEFI_IMPLEMENTATION.md))
- ✅ Basic VGA text terminal + framebuffer console
- ✅ Initial identity mapping (transitory)
- ✅ Working stack
- ✅ Interrupt Descriptor Table (IDT)
- ✅ PIT timer with periodic interrupts (IRQ0)
- ✅ Tick and uptime system
- ✅ Blocking sleep functions
- ✅ PS/2 keyboard driver with circular buffer (IRQ1)
- ✅ Physical Memory Manager (PMM) frame allocator
- ✅ Heap allocator (kmalloc/kfree) with dynamic expansion
- ✅ Virtual Memory Manager (VMM) with user space support and in-space translation
- ✅ NX Bit and W^X policy for kernel regions and ELF segments
- ✅ ELF64 loader (PT_LOAD segments, W^X enforcement, p_align handling, per-process page tracking)
- ✅ User process address spaces + stack with guard page
- ✅ Extended PCB (state, registers, manifest, mapped page list for precise unload)
- ✅ Multiboot memory map parsing
- ✅ Interactive shell with commands
- ✅ Error handling during boot & process unload (elfunload)
- ✅ ps command (basic process listing)

See our [Development Roadmap](ROADMAP_SECoS.md) for upcoming features!

## Build — UEFI Path (primary)

### Requirements

```bash
sudo apt install nasm gcc binutils gnu-efi ovmf qemu-system-x86 mtools
```

### Build kernel + UEFI bootloader

```bash
make          # produces kernel.bin
make uefi     # produces dist/EFI/BOOT/BOOTX64.EFI
```

### Update ESP image

```bash
cp kernel.bin dist/kernel.elf
mcopy -i dist/test_esp.img -o dist/kernel.elf ::kernel.elf
mcopy -i dist/test_esp.img -o dist/EFI/BOOT/BOOTX64.EFI ::EFI/BOOT/BOOTX64.EFI
```

### UEFI smoke test (canonical command, M1+)

Build ESP image and run with q35 (OVMF works best with q35 chipset):

```bash
# Full rebuild
make clean && make && make uefi

# Rebuild ESP image (run once or after EFI/kernel changes)
dd if=/dev/zero of=dist/test_esp.img bs=1M count=64 2>/dev/null
mkfs.fat -F 32 dist/test_esp.img
mmd -i dist/test_esp.img ::/EFI ::/EFI/BOOT
mcopy -i dist/test_esp.img dist/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy -i dist/test_esp.img dist/kernel.elf ::/kernel.elf

# Run (exit 124 = timeout = kernel alive; exit 0 = kernel crashed)
timeout 30 qemu-system-x86_64 \
  -machine q35,accel=tcg \
  -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=dist/OVMF_VARS_test.fd \
  -drive if=ide,format=raw,file=dist/test_esp.img \
  -net none \
  -nographic \
  -serial file:/tmp/secos_smoke.log
echo "Exit: $?"
```

Serial output lands in `/tmp/secos_smoke.log`.

### Smoke test (canonical, both paths)

```bash
# Multiboot2 / GRUB (build ISO + run 20s)
tools/smoke.sh --mb2 --timeout 20 --log /tmp/secos_mb2.log

# UEFI / OVMF / q35 (build + run 25s)
tools/smoke.sh --uefi --timeout 25 --log /tmp/secos_uefi.log
```

Exit 0 = PASS (kernel alive). Exit 1 = FAIL (triple fault / crash).
Debugcon log captures: build timestamp, git hash, M2 stack switch RSP.

### Run with QEMU (legacy command, pre-M1)

```bash
qemu-system-x86_64 \
  -machine pc -cpu qemu64 -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=dist/OVMF_VARS_test.fd \
  -drive if=ide,format=raw,file=dist/test_esp.img \
  -serial file:/tmp/kernel_debug.log \
  -debugcon file:/tmp/uefi_boot.log \
  -display none -no-reboot
```

## Build — Multiboot2 / GRUB Path (legacy)

### Requirements

```bash
sudo apt install nasm gcc binutils grub-common grub-pc-bin xorriso qemu-system-x86
```

### Build ISO and run

```bash
make iso         # produces myos.iso (GRUB bootable)
make run         # build ISO + run in QEMU (graphical window)
make run-serial  # build ISO + run headless: interact with the shell in THIS terminal over COM1
make run-vnc     # build ISO + run with a VNC server on localhost:5900
make clean       # remove all artifacts
```

### Headless console (COM1 serial)

The interactive shell is usable without a graphical window: terminal output is
mirrored to COM1 and serial RX feeds the shell input path, so

```bash
make run-serial   # qemu ... -serial stdio -display none
```

drives the whole shell in the terminal (useful where WSLg/GTK/SDL do not work).
Quit QEMU with `Ctrl-A` then `X`. The framebuffer window (`make run`) and VNC
(`make run-vnc`) remain available.

## Boot Architecture

```
UEFI path (primary):
  OVMF → BOOTX64.EFI → efi_main() → elf_load_kernel()
       → ExitBootServices() → _uefi_start [BITS 64] → kernel_main(magic=0, info=&secos_boot_info)

Multiboot2 path (legacy):
  GRUB → _start [BITS 32] → long mode setup → long_mode [BITS 64] → kernel_main(magic=0x36d76289, info=mb2_info)
```

`kernel_main` distinguishes the boot path via `magic`:
- `magic == 0` → UEFI; `info` is a pointer to `struct secos_boot_info`
- `magic == 0x36d76289` → Multiboot2; `info` is pointer to Multiboot2 info struct

## Project Structure (simplified)

```
.
├── boot/
│   └── boot.asm          # Multiboot2 entry (_start, 32-bit) + UEFI entry (_uefi_start, 64-bit)
├── arch/x86/
│   ├── idt_asm.asm        # Interrupt stubs
│   └── syscall_asm.asm    # Syscall entry
├── kernel/
│   ├── kernel.c           # kernel_main, initialization
│   └── bootinfo.h         # secos_boot_info struct (shared UEFI/Multiboot2)
├── uefi/
│   ├── boot.c             # UEFI bootloader (efi_main)
│   ├── elf_load.c         # ELF64 loader (used by UEFI bootloader)
│   ├── efi.h              # UEFI type definitions
│   └── crt0.s             # UEFI application CRT0 (replaces gnu-efi crt0)
├── mm/                    # PMM, VMM, heap, ELF kernel loader
├── drivers/               # Framebuffer, keyboard, timer, RTC
├── fs/                    # RAMFS, VFS, FAT32, ext2
├── lib/                   # Terminal utilities
├── user/                  # Embedded test ELF driver
├── dist/
│   ├── EFI/BOOT/BOOTX64.EFI   # Built UEFI bootloader
│   ├── test_esp.img            # FAT32 ESP image for QEMU
│   └── OVMF_VARS_test.fd       # UEFI variable store
├── linker.ld              # Kernel linker script
└── Makefile               # Build system
```

## Shell Commands

Once the kernel boots you can use these commands:

- **help** - Show list of available commands
- **clear** - Clear the screen
- **echo [text]** - Print specified text
- **info** - Show system information
- **uptime** - Display system uptime
- **sleep [ms]** - Wait N milliseconds (1-10000)
- **mem** - Show memory statistics (PMM + Heap)
- **memtest** - Memory allocation/free test
- **memstress** - Heap allocator stress
- **elfload** - Load embedded test ELF
- **elfunload** - Destroy last loaded process
- **ps** - List active processes (minimal)
- **colors** - VGA color test
- **reboot** - Reboot system

### RAMFS (In-Memory Filesystem)

The kernel includes a simple in-memory filesystem supporting mutable/immutable files and hierarchical directories. Main commands:

- **rfls [path]** - List direct children of a directory (root if omitted)
- **rfcat <file>** - Show file content
- **rfinfo <file>** - Show metadata + first bytes
- **rfadd <file> <content>** - Create mutable file with initial content
- **rfwrite <file> <offset> <data>** - Write (grows if needed) into mutable file
- **rfdel <file>** - Delete mutable file
- **rfmkdir <dir>** / **rfrmdir <dir>** - Create / remove directory (empty)
- **rfcd <path>** / **rfpwd** - Change / show RAMFS working directory
- **rftree [path]** - Print recursive tree with branch format (├─, └─)
- **rfusage** - Stats: number files, dirs, total bytes, free slots
- **rfmv <old> <new>** - Rename file or directory (updates descendant paths). Prevents cycles (cannot rename a dir inside itself: /a -> /a/b).
- **rftruncate <file> <size>** - Shrink/expand mutable file to size bytes

Special files generated at boot:

- `VERSION` - Dynamic build info (BUILD_TS and GIT_HASH from Makefile)
- `init.rc` - Command script auto-executed at boot (uses shell_run_line)
- `sys/syscalls.txt` - Placeholder syscall list
- `sys/manifest.txt` - Initial RAMFS manifest (list TYPE SIZE NAME of each entry)

Path notes: resolver normalizes paths removing duplicate slashes and handling `.` and `..`.

Quick example:
```
rfmkdir docs
rfadd docs/readme.txt HelloRAMFS
rfwrite docs/readme.txt 5 _World
rfcat docs/readme.txt     # Output: Hello_World
rfmv docs/readme.txt docs/info.txt
rftree docs
rftruncate docs/info.txt 5
rfcat docs/info.txt       # Output: Hello
```

### VFS (Virtual File System)

A minimal VFS layer abstracts different filesystems under a unified API.
Main components:
- `vfs_mount_root()` – mount a filesystem as root `/` (currently RAMFS).
- `vfs_lookup(path)` – resolve a generic inode (file or directory).
- `vfs_readdir(path, cb)` – iterate direct children of a directory.
- File ops: `vfs_read_all`, `vfs_write`, `vfs_create`, `vfs_truncate`, `vfs_remove`, `vfs_rename`, `vfs_mkdir`.

Shell VFS commands:
- `vls [path]` – list via VFS (shows absolute paths with leading `/`).
- `vcat <path>` – read file through the VFS layer.
- `vinfo <path>` – show type and size.
- `vpwd` – show CWD (reuses RAMFS working dir for now).
- `vmount` – show mount root status (RAMFS already mounted).

Planned next steps:
- FAT32 driver: parse BPB, FAT table and root directory (initial read-only).
- exFAT driver: different structure (allocation bitmap + up-case table); initial read-only support.
- Abstract block device interface (e.g. `block_read(sector, buf)`). Under QEMU can be simulated with a buffer or future ATA/virtio.

Design file in preparation: `FAT32.md` will outline initial parsing and inode VFS mapping.

### Driver Space (SYS_DRIVER)

Introduced a mediation subsystem for device access named **Driver Space**. It allows a user process to register as a "driver" for a device and perform granular operations via the `SYS_DRIVER` syscall, without exposing raw MMIO or sensitive structures.

Componenti principali:
 - `device_desc_t` – descriptor with register and memory region base/size plus capability mask.
 - Device registry – array initialized by `driver_registry_init()`.
 - Process→device binding – created with shell command `drvreg <id>`.
 - `driver_call_t` – request structure sent to `driver_syscall()` (user-space wrapper):
   ```c
   typedef struct {
         uint32_t opcode;     // DRIVER_OP_*
         uint32_t device_id;  // indice nel registro
         uint64_t reg_offset; // offset nel buffer registri
         uint64_t value;      // valore per WRITE_REG o arg generico
         uint64_t mem_offset; // offset regione memoria device
         uint64_t mem_length; // lunghezza per map/unmap
         uint32_t flags;      // DRIVER_FLAG_*
   } driver_call_t;
   ```
- Dispatcher kernel `handle_driver_call()` – valida e esegue l'operazione.
- Permission engine – verifica binding, capability e range.
- Audit log – buffer circolare con eventi (errori o flag audit).

Initial supported opcodes:
- `DRIVER_OP_READ_REG`
- `DRIVER_OP_WRITE_REG`
- `DRIVER_OP_MAP_MEM` (stub)
- `DRIVER_OP_UNMAP_MEM` (stub)
- `DRIVER_OP_GET_INFO`

Primary result codes: `DRV_OK`, `DRV_ERR_DEVICE`, `DRV_ERR_BINDING`, `DRV_ERR_OPCODE`, `DRV_ERR_RANGE`, `DRV_ERR_PERM`, `DRV_ERR_ARGS`.

Quick user-space example (testdriver):
```c
driver_call_t dc = {0};
dc.device_id = 0;
dc.opcode = DRIVER_OP_WRITE_REG;
dc.reg_offset = 0x4;
dc.value = 0xABCD1234;
long r = driver_syscall(&dc);
```

Related shell commands:
- `drvinfo` – show devices and bindings.
- `drvreg <id>` / `drvunreg <id>` – bind / remove process binding.
- `drvlog` – print audit log.
- `drvtest` – execute a test operation sequence.

Current security:
- No direct access to real MMIO (shadow register buffer).
- Capability gating reduces surface.
- Exclusive binding per device.
- Audit errors always recorded.

Future hardening plans: rate limiting per process/opcode, advanced audit filter (`drvlog errors|dev=<n>|op=<code>`), DMA sandbox, IRQ subscribe, bulk transfer.

For more details see `DRIVER_IF.md`.

## Capabilities

- ✅ Long Mode (64-bit) boot
- ✅ Basic VGA text terminal
- ✅ Identity mapping of first 1GB of memory
- ✅ Working stack
- ✅ Error handling during boot

## Test on real hardware

To test on real hardware:

1. Write the ISO to a USB flash drive:
   ```bash
   sudo dd if=myos.iso of=/dev/sdX bs=4M status=progress
   ```
   (Replace `/dev/sdX` with the correct device)

2. Boot the computer from the USB drive

## Future Developments

This kernel is a solid starting point. You can extend it with:
 - **Ring-3 transition + context switch** - in progress (M5–M7): trapframe-based syscall entry, `arch_iret_to_tf`, and `SYS_YIELD` cooperative scheduling are implemented; the ring-3 demo does not yet complete (see Current Status)
 - **Preemptive scheduler** - timer-driven (IRQ0) context switching on top of the M6/M7 trapframe machinery
 - **Security manifest** - Parse `.note.secos` section for policy (loader support already present; see below)
## Security Manifest (.note.secos)

The loader searches for an ELF note (PT_NOTE) with name `SECOS` and type `QSEC` containing a structure:
```
uint32_t version;
uint32_t flags;   // MANIFEST_FLAG_REQUIRE_WX_BLOCK, STACK_GUARD, NX_DATA, RX_CODE
uint64_t max_mem; // limite attivo: se usage > max_mem abort
uint64_t entry_hint; // entry attesa (0 = ignora)
```
If present it is validated (entry match, supported flags). W|X segments are rejected unconditionally. The max_mem field is compared to total occupied memory (pages * 4096) after loading and before process start: if it exceeds the limit the process is aborted.
 - **ASLR** - Address space layout randomization for code and stack
 - **File system** - FAT32/exFAT + VFS
 - **File system** - RAM or disk-based filesystem
 - **Device drivers** - Mouse, serial port, AHCI/IDE
 - **Networking** - Basic TCP/IP stack
 - **Advanced shell** - Piping, redirection, job control
 - **Syscalls** - User/kernel space interface

## Debugging

Per debug con QEMU e GDB:
```bash
qemu-system-x86_64 -cdrom myos.iso -s -S
```

In another terminal:
```bash
gdb kernel.bin
(gdb) target remote localhost:1234
(gdb) continue
```

## Useful Resources

- [OSDev Wiki](https://wiki.osdev.org/) - Comprehensive OS development resource
- [Intel Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) - Intel CPU documentation
- [AMD Manual](https://www.amd.com/en/support/tech-docs) - AMD CPU documentation
- [GRUB Documentation](https://www.gnu.org/software/grub/manual/) - GRUB manual

## License

This code is provided as an educational example and may be used freely under the MIT license (see `LICENSE.md`).

## Security Model: Kernel / Driver Space / User Space

SECoS adotta un modello a tre livelli logici. Il **Kernel Space** (Ring 0) possiede page tables,
PMM, VMM, IDT e scheduler. Lo **User Space** (Ring 3) esegue processi ELF isolati, senza accesso
hardware diretto. Il **Driver Space** è un livello intermedio in Ring 3: un processo con binding a
un dispositivo può eseguire operazioni granulari via `SYS_DRIVER`, mediate e auditate dal kernel.
Nessun processo Ring 3 accede a MMIO, IOPL=0. Ogni operazione driver è soggetta a capability check
(`caps_mask`) e registrata nel log circolare.

Specifica completa: [`docs/DRIVER_SPACE.md`](docs/DRIVER_SPACE.md).

## Memory & Security Notes

The kernel applies W^X to its sections and marks data regions NX. User pages are mapped with USER while shared kernel regions keep USER=0 after hardening (`vmm_harden_user_space`). The user stack has an unmapped guard page to catch overflow via page fault. The ELF loader enforces that no segment is both writable and executable and validates alignment (p_align 0 or 0x1000). Every code/data/stack page is tracked in the PCB for precise unload and memory accounting (manifest max_mem).

## Glossary

| Term | Definition |
|------|------------|
| Long Mode | 64-bit operating mode of x86-64 CPUs enabled via EFER.LME and paging. |
| Multiboot | Boot specification allowing GRUB to pass memory and module info to the kernel. |
| IDT | Interrupt Descriptor Table: maps interrupt vectors to handler entry points. |
| PIC | Programmable Interrupt Controller (8259) remapped to avoid conflict with CPU exceptions. |
| PIT | Programmable Interval Timer generating periodic IRQ0 ticks (used for uptime and future scheduling). |
| PMM | Physical Memory Manager allocating/releasing 4KB frames using a bitmap. |
| Heap Allocator | Dynamic memory allocator (kmalloc/kfree) expanding with PMM frames. |
| VMM | Virtual Memory Manager handling page mapping, user spaces, physmap and hardening (USER bit removal for kernel regions). |
| Physmap | High virtual linear mapping of all physical memory for direct access (non-executable). |
| W^X | Policy ensuring writable pages are not executable (Write XOR Execute). |
| NX Bit | No-Execute bit set on data regions to block instruction fetches. |
| Guard Page | Unmapped page placed adjacent to a stack to catch overflow via page fault. |
| ELF Loader | Loads ELF64 binaries (PT_LOAD segments) enforcing alignment and W^X restrictions. |
| Manifest (SECOS note) | ELF PT_NOTE section carrying security flags (stack guard, NX data, RX code requirement, max memory). |
| PCB | Process Control Block containing PID, registers, memory space pointer, stack top, manifest pointer and accounting fields. |
| RAMFS | In-memory hierarchical filesystem supporting mutable and immutable entries. |
| VFS | Virtual File System abstraction layering over RAMFS and future FS drivers (ext2, FAT32). |
| Driver Space | Mediated interface allowing user processes to perform granular device operations via audited syscall. |
| Capability Mask | Bitmask indicating which driver operations (read/write/map/info) a device permits. |
| Audit Log | Circular buffer capturing driver syscall events (tick, pid, opcode, result) for inspection. |
| IST | Interrupt Stack Table entries used to switch to dedicated stacks for critical faults (e.g., double fault). |
| CR3 | Register holding base of current paging structures (PML4 in x86-64). |
| PML4 | Top-level page table in x86-64 used in 4 or 5 level paging hierarchies. |
| Phys Address | Real hardware memory address before translation; often accessed via physmap. |
| USER bit | Page table permission bit allowing code/data access from ring3 (future user-mode). |
| Trap Gate | IDT entry type preserving IF flag state for syscalls/int instructions (as used for INT 0x80). |
| Tick | Increment from PIT interrupt used for timekeeping and scheduling. |
| Frame | 4KB physical memory unit managed by PMM. |
| Huge Page | Larger page (2MB) used to reduce page table overhead for physmap. |
| Demand-Zero | Technique allocating zeroed pages lazily on first access within a registered region. |

