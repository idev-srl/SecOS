# 64-bit Kernel with GRUB

A kernel written in C that boots in Long Mode (64-bit) via GRUB as the bootloader, with PS/2 keyboard support and an interactive shell.

## Features

- ✅ Long Mode (64-bit) boot
- ✅ Multiboot support (GRUB)
- ✅ Basic VGA text terminal
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

## Requirements

### Required software:
- **NASM** - Assembler for boot code
- **GCC** - C compiler (cross-compilation for x86_64 recommended)
- **GNU ld** - Linker
- **GRUB** - To build the bootable ISO image
- **xorriso** - Required by grub-mkrescue
- **QEMU** - To test the kernel (optional but recommended)

### Install on Ubuntu/Debian:
```bash
sudo apt update
sudo apt install nasm gcc binutils grub-common grub-pc-bin xorriso qemu-system-x86
```

### Install on Arch Linux:
```bash
sudo pacman -S nasm gcc binutils grub xorriso qemu
```

## Build

## Project Structure (simplified)

```
.
├── boot.asm      # Assembly to enter long mode
├── idt_asm.asm   # Interrupt handlers in assembly
├── kernel.c      # Main kernel code
├── idt.c/h       # Interrupt Descriptor Table management
├── timer.c/h     # PIT (Programmable Interval Timer) driver
├── keyboard.c/h  # PS/2 keyboard driver
├── multiboot.h   # Multiboot standard structures
├── pmm.c/h       # Physical Memory Manager
├── heap.c/h      # Heap allocator
├── vmm.c/h       # Virtual Memory Manager + user spaces
├── elf.c/h       # ELF64 loader
├── process.c/h   # Process creation (PCB)
├── shell.c/h     # Interactive shell
├── terminal.h    # Shared VGA terminal API
├── linker.ld     # Linker script
├── Makefile      # Build script
└── README.md     # This file
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

## Build Requirements

### Compile the kernel:
```bash
make
```
This command produces `kernel.bin`.

### Create the ISO image:
```bash
make iso
```
This creates `myos.iso`, a bootable GRUB ISO.

### Run with QEMU:
```bash
make run
```
This compiles, builds the ISO and runs the kernel in QEMU.

### Clean generated files:
```bash
make clean
```

## How it works

### 1. Boot Process (boot.asm)
- GRUB loads the kernel in 32-bit protected mode
- Code checks CPUID and Long Mode support
- Sets up page tables for 64-bit paging (4MB identity mapping)
- Enables PAE (Physical Address Extension)
- Configures the EFER MSR to enable long mode
- Enables paging
- Jumps to 64-bit code

### 2. IDT Initialization (idt.c)
- Creates the Interrupt Descriptor Table with 256 entries
- Remaps the PIC (Programmable Interrupt Controller)
- Sets up the handler for IRQ0 (timer)
- Sets up the handler for IRQ1 (keyboard)
- Enables interrupts

### 3. PIT Timer (timer.c)
- Configures the Programmable Interval Timer at 1000 Hz (1ms per tick)
- Handles timer interrupts (IRQ0)
- Maintains a 64-bit tick counter
- Provides uptime and blocking sleep functions
- Foundation for future scheduling

### 4. Keyboard Driver (keyboard.c)
- Handles keyboard interrupts (IRQ1)
- Converts PS/2 scancodes to ASCII characters
- Supports Shift, Caps Lock and special characters
- Circular buffer for input
- Blocking and non-blocking functions to read characters

### 5. Shell (shell.c)
- Main loop reading user commands
- Simple parser splitting command and arguments
- Executes built-in commands
- Colored prompt and backspace handling

### 6. Kernel (kernel.c)
- Initializes the VGA text terminal
- Prints boot information
- Verifies the Multiboot magic number
- Initializes IDT, timer, keyboard and shell
- Transfers control to the interactive shell

## Capabilities (legacy list)

- ✅ Long Mode (64-bit) boot
- ✅ Multiboot support for GRUB
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
 - **Preemptive scheduler** - Context switching using PCB.regs
 - **Ring3 transition** - Syscall gate and TSS.rsp0
 - **Security manifest** - Parse `.note.secos` section for policy
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
