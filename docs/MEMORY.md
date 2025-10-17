# Memory Management in SecOS

## 📋 Architecture

SecOS memory management is divided into two layers:

### 1. Physical Memory Manager (PMM)
Manages physical memory at the **frame** level (each 4KB).

**Features:**
- Uses a **bitmap** to track free/allocated frames
- Each bit represents a 4KB frame
- Parses Multiboot memory map to detect RAM
- Simple API: `pmm_alloc_frame()` / `pmm_free_frame()`

**Initialization:**
```c
pmm_init(multiboot_info);  // Called by kernel_main
```

**Usage:**
```c
void* frame = pmm_alloc_frame();  // Allocate a 4KB frame
if (frame) {
  // Use the frame...
  pmm_free_frame(frame);        // Free when done
}
```

### 2. Heap Allocator
Provides dynamic allocation of **variable-sized memory**.

**Features:**
- API similar to malloc/free
- Uses PMM internally to expand the heap
- Free block list with coalescing
- Header per block (size + flags)

**Usage:**
```c
char* buffer = kmalloc(1024);     // Allocate 1KB
if (buffer) {
  // Use buffer...
  kfree(buffer);                 // Free when done
}

// Aligned allocation
void* aligned = kmalloc_aligned(size, 16);  // 16-byte aligned
```

## 🔍 Comandi Shell
## Virtual Memory Manager (VMM)

The VMM introduces:
* Basic functions: `vmm_init`, `vmm_map`, `vmm_unmap`, `vmm_translate`, `vmm_alloc_page`.
* User space functions (separate address spaces): `vmm_space_create_user`, `vmm_map_in_space`, `vmm_alloc_page_in_space`, `vmm_map_user_code_in_space`, `vmm_alloc_user_page_in_space`, `vmm_alloc_user_stack_in_space`, `vmm_translate_in_space`, `vmm_harden_user_space`.
* Physmap: maps all available physical memory in a high range (`VMM_PHYSMAP_BASE = 0xFFFF888000000000`) using 2MB huge pages to reduce table count.
* Region allocator (stub): register virtual regions for future on-demand allocations (user heap, stack, FS cache).
* Page Fault handler: if a page is absent and the address falls into a registered region, it is allocated (demand-zero). Otherwise panic.

### W^X + NX
The kernel applies a "Write XOR Execute" policy to its regions:
* `.text` → RX
* `.rodata` → R (NX)
* `.data` / `.bss` → RW (NX)
* Kernel stack → RW (NX)
* User code pages → RX (USER)
* User data/stack pages → RW (NX | USER)

NX is enabled by setting EFER.NXE in `boot.asm`. The ELF loader rejects segments requesting W|X simultaneously.

### User Stack with Guard Page
User stack is allocated as N mapped pages below `USER_STACK_TOP` and one unmapped page immediately below to detect overflow (page fault if crossed). Managed by `vmm_alloc_user_stack_in_space`.

### User Address Space
Each process starts with a clone of the kernel PML4 then "hardened" via `vmm_harden_user_space` removing the USER bit from high kernel regions to prevent accidental access. User pages carry USER bit and only those will be accessible in ring3 (future).
During creation (`vmm_space_create_user`) PDPT entries for the user range (CODE/DATA/STACK) are zeroed to avoid inherited huge mappings or filled tables causing collisions when the ELF loader maps new pages (prevents the "map failed" error in subsequent `elfload` calls).

### In-Space Translation
To operate on pages of an inactive address space use `vmm_translate_in_space` which resolves the virtual address against the other space tables, useful for copying ELF segments and zeroing BSS.

### Unmapping Pages in a Space
API `vmm_unmap_in_space(space, virt)` removes a page from a user space without switching CR3, freeing the physical frame and leaving other spaces intact. Used by `elf_unload_process` to release code/data/stack pages.

### PCB (Process Control Block) Memory Fields
PCB contains:
* `space` → pointer to its address space
* `entry` → entry point (initial RIP)
* `stack_top` → user stack top
* `kstack_top` → (stub) kernel stack for future ring3 transitions
* `regs` → initial register snapshot (RIP/RSP/RFLAGS etc.)
* `manifest` → pointer to security descriptor (stub not yet used)

### Proposed Virtual Layout
| Area | Description |
|------|-------------|
| Low (< 16MB) | Initial identity kernel (transient) |
| Physmap (high) | Direct physical mapping (non-executable) |
| Kernel heap | TBD, 4KB pages RW |
| User space | User code/data separated via USER bit |
| FS cache | FAT32/exFAT block buffers |
| Guard pages | Unmapped pages to detect overflow |

### Next Steps
1. Complete migration to higher-half removing initial identity mapping.
2. Advanced region allocator (merge/fragmentation) + demand paging for user heap.
3. Block cache (LRU) and block device abstraction.
4. Syscall gate and ring3 transition with TSS.rsp0 (use `kstack_top`).
5. ELF security manifest (`.note.secos`) with extended W^X enforcement.
6. Scheduler + context switch saving `regs`.


### mem
Shows complete memory statistics:
```bash
secos$ mem

  Total memory:     128 MB
  Used memory:      2 MB
  Free memory:      126 MB

=== Heap Statistics ===
Allocated:  5120 bytes
Freed:      2048 bytes
In use:     3072 bytes
```

### memtest
Performs allocation/deallocation test:
```bash
secos$ memtest

Memory allocation test...
Test 1: Allocating 5 blocks of 1KB...
  [OK] Block 1 allocated
  [OK] Block 2 allocated
  ...
Test 2: Freeing blocks...
  [OK] Block 1 freed
  ...
Test completed!
```

## 🛠️ Full API

### PMM (Physical Memory Manager)

```c
// Initialize PMM
void pmm_init(void* mboot_info);

// Allocate a 4KB physical frame
void* pmm_alloc_frame(void);

// Free a physical frame
void pmm_free_frame(void* addr);

// Statistics
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_used_memory(void);
uint64_t pmm_get_free_memory(void);
void pmm_print_stats(void);
```

### Heap Allocator

```c
// Initialize heap
void heap_init(void);

// Allocate dynamic memory
void* kmalloc(size_t size);

// Allocate aligned memory
void* kmalloc_aligned(size_t size, size_t alignment);

// Free memory
void kfree(void* ptr);

// Statistics
void heap_print_stats(void);
```

## 📊 Layout Memoria

```
0x00000000  +------------------+
            | BIOS / HW        |
0x00100000  +------------------+
            | Kernel Code      | ← Loaded by GRUB at 1MB
            | Kernel Data      |
            | Kernel BSS       |
_kernel_end +------------------+
            | PMM Bitmap       | ← Frame allocation bitmap
            +------------------+
            | Heap             | ← Grows dynamically
            +------------------+
            | Free frames      | ← Managed by PMM
            |                  |
            |    ...           |
            +------------------+
```

## 🚀 Next Steps

To load user-space applications we introduced:

1. **ELF Loader** (base) → loads PT_LOAD segments with W^X and alignment checks.
2. **Address Space** → created per process with dedicated user stack and guard page.
3. **PCB** → minimal structure for identity and initial registers.

Incoming:
* Syscalls
* Scheduler
* Manifest parser
* File system

## 💡 Note Implementative

### Bitmap Allocator
- Efficiente per allocazioni di frame singoli
- O(n) per trovare frame libero (accettabile)
- Compatto in memoria (1 bit per frame)

### Heap Allocator
- First-fit algorithm
- Automatic coalescing of free blocks
- Automatic heap expansion via PMM
- 24-byte header per block

### Current Limitations
- Heap cannot shrink (only grow)
- No protection against double-free
- No advanced fragmentation handling
- Allocations > 4KB allocate multiple frames

### Possible Improvements
- Buddy allocator for PMM
- Slab allocator for small objects
- Guard pages to detect overflow
- Memory pools for frequent allocations