# M3 Security Audit — Kernel/User Isolation

**Branch:** `milestone/M1` (M3 commits applied on top of M2_STABLE)
**Scope:** Memory mapping flags, syscall path, user pointer safety, address space separation.
**Code read:** M2_STABLE tag.
**No code modified in this document.**

---

## 1. Kernel Memory Mapping — Supervisor (U/S) Flag

### 1.1 Identity mapping (0–512 MB, PML4[0])

Built by `vmm_build_kernel_pml4_identity_512mb()` called from `vmm_init()`.
Uses 2 MB huge pages, flags: `PRESENT | RW` — **no VMM_FLAG_USER**.

| Region | VA | Flags | U/S |
|--------|----|-------|-----|
| Identity map | 0x0 – 0x1FFFFFFF | PRESENT \| RW \| PS | 0 (supervisor) |

**Finding:** Kernel identity map is supervisor-only. ✓

### 1.2 Physmap (0xFFFF888000000000, PML4[272])

Built by `vmm_init_physmap()`. Uses 2 MB huge pages,
flags: `PRESENT | RW | VMM_FLAG_NOEXEC` — **no VMM_FLAG_USER**.

**Finding:** Physmap is supervisor-only. ✓

### 1.3 M2 Stack region (0xFFFFFF8000000000, PML4[511])

Built by `vmm_alloc_kernel_stack()` and `vmm_alloc_ist_stack()`.
Calls `m2_map_stack_pages()` → `vmm_map(va, frame, VMM_FLAG_RW | VMM_FLAG_NOEXEC)` — **no VMM_FLAG_USER**.

**Finding:** Kernel/IST stacks are supervisor-only. ✓

### 1.4 vmm_map() enforcement

`vmm_map()` (`mm/vmm.c:729`) accepts arbitrary flags from caller with no validation.
No check prevents a kernel VA from being mapped with `VMM_FLAG_USER`.

**Finding:** No policy enforcement in `vmm_map()`. Caller discipline assumed. — **MEDIUM**

### 1.5 vmm_harden_user_space() — broken

`vmm_harden_user_space()` (`mm/vmm.c:820`) attempts to remove USER bit from
kernel-range PML4 entries. The check is:

```c
uint64_t virt_base = ((uint64_t)i << 39);
if (virt_base >= 0xFFFF800000000000ULL) pml4[i] = e & ~VMM_FLAG_USER;
```

**Bug:** `((uint64_t)i << 39)` for `i = 256..511` gives `0x0000800000000000`–`0x0000FF8000000000`,
which is **never** `>= 0xFFFF800000000000`. The condition is **always false**. The function is
effectively a no-op.

Additionally, the PML4 pointer uses the physical address as a virtual address without `phys_to_virt()`,
which works only because kernel frames are identity-mapped (below 512 MB). Technically incorrect.

**Finding:** `vmm_harden_user_space()` does nothing. — **MEDIUM**
(Impact is limited because `vmm_space_create_user()` already strips USER from all shared entries.)

---

## 2. User Memory Mapping

### 2.1 User code pages

`map_user_page(virt, rw=0, exec=1)` → `VMM_FLAG_PRESENT | VMM_FLAG_USER` (no RW, no NX).
`map_user_page(virt, rw=1, exec=0)` → `VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_RW | VMM_FLAG_NOEXEC`.

**Finding:** User code RX, user data RW/NX. W^X enforced at load time. ✓

### 2.2 User stack

`vmm_alloc_user_stack_in_space()` → `vmm_alloc_user_page_in_space()` → `map_user_page_in_space()`.
Flags: `PRESENT | USER | RW | NOEXEC`. Guard page (NP) installed below stack. ✓

### 2.3 NX on user data/stack

`VMM_FLAG_NOEXEC` (bit 63) set on all data/stack pages. ✓
NX bit requires `EFER.NXE` enabled at boot (confirmed by working physmap NOEXEC mappings).

---

## 3. Syscall Path

### 3.1 Entry assembly (`arch/x86/syscall_asm.asm`)

```
INT 0x80 → IDT entry (Trap Gate) → CPL check (ring3 → ring0) → CPU stack switch via TSS.rsp0
         → syscall_entry: push callee-saved regs, call syscall_dispatch, pop, iretq
```

The `iretq` returns to ring 3. TSS.rsp0 holds the top of the M2 guarded kernel stack
(set in `tss_init()`). The CPU uses TSS.rsp0 to switch the stack on ring-3→ring-0 transition.

**Finding:** Stack switch is correct. CPU enforces RSP switch. No user stack used in ring-0.

However, `syscall_entry` does **not** validate register values before passing them to
`syscall_dispatch`. All `a0..a4` are raw user-supplied values. ✓ (expected; dispatcher must validate)

### 3.2 Syscall dispatcher (`kernel/syscall.c`)

```c
case SYS_OPEN:   return (uint64_t)ksys_open((const char*)a0, (int)a1);
case SYS_READ:   return (uint64_t)ksys_read((int)a0,(void*)a1,(int)a2);
case SYS_WRITE:  return (uint64_t)ksys_write((int)a0,(const void*)a1,(int)a2);
case SYS_DRIVER: return (uint64_t)driver_syscall((struct driver_call*)a0);
```

**Finding:** All user pointers (path, buffer, driver struct) are **cast directly and passed to kernel
functions without range validation**. No `copy_from_user`. — **HIGH**

### 3.3 TSS.rsp0

Set to `M2_KSTACK_TOP` in `tss_init(uint64_t kernel_rsp0)`. Value is `0xFFFFFF8000005000`.
Updated on every context switch (not implemented yet — single process only). ✓

---

## 4. User Pointer Dereference Analysis

### 4.1 SYS_OPEN — `ksys_open(const char* path, ...)`

`path` is a user pointer. Passes directly to `vfs_lookup(path)` which reads bytes from `path`
until NUL. **No range check, no copy.** — **HIGH**

If `path = 0xFFFF888000000000` (kernel physmap base), the kernel would read from its own physmap,
potentially leaking physical memory contents.

### 4.2 SYS_READ / SYS_WRITE — `ksys_read/write(fd, buf, len)`

`buf` is a user pointer. Passes to `ino->ops->read(ino, off, buf, len)` or
`ino->ops->write(ino, off, buf, len)`. VFS handlers read/write directly through `buf`.
**No range check.** — **HIGH**

User can supply `buf = 0xFFFF888000000000` to read or overwrite arbitrary physical memory.

### 4.3 SYS_DRIVER — `driver_syscall(struct driver_call* req)`

`req` is a user pointer. Struct fields `device_id`, `opcode`, `target`, `value`, `flags` are
read directly. **No range check on the pointer.** — **HIGH**

`handle_driver_call()` writes back `req->value` with register data (READ_REG result).
User can point `req` at kernel memory to overwrite arbitrary kernel data. — **CRITICAL**

### 4.4 Summary of dangerous dereferences

| Location | Pointer | Direction | Severity |
|----------|---------|-----------|----------|
| `syscall.c:19` (SYS_OPEN) | `(char*)a0` path | read | HIGH |
| `syscall.c:21` (SYS_READ) | `(void*)a1` buf | write | HIGH |
| `syscall.c:22` (SYS_WRITE) | `(void*)a1` buf | read | HIGH |
| `syscall.c:23` (SYS_DRIVER) | `(driver_call_t*)a0` struct | read+write | CRITICAL |
| `driver_if.c:119` | `req->value=val` writeback | write | CRITICAL |

---

## 5. Address Space per Process

### 5.1 Per-process PML4

`vmm_space_create_user()` allocates a **fresh PML4 frame** per process. ✓
Each process has its own `vmm_space_t` with unique `pml4_phys`. CR3 is per-process.

### 5.2 Kernel entry sharing (PML4[256+])

The new PML4 is initialized by copying **all** kernel PML4 entries with USER stripped:

```c
for (int i=0;i<PT_ENTRIES;i++) {
    uint64_t e = old_pml4[i];
    if (e & VMM_FLAG_PRESENT) new_pml4[i] = e & ~VMM_FLAG_USER;
}
```

PML4 entries 256–511 (kernel high half) are copied with U=0. CPU cannot access them from ring-3. ✓

**Finding:** PML4[256..511] in user space have U=0 → kernel not reachable from ring-3. ✓

**Caveat:** This relies on kernel pages never being mapped with VMM_FLAG_USER. The current
`vmm_map()` has no enforcement of this. (See 1.4.)

### 5.3 User process kernel modification

A user process shares kernel PDPT/PDT/PT tables via PML4[0..255] (read with U=0).
It cannot modify these tables from ring-3 (U=0 blocks all ring-3 access, including writes).
The shared PDPT for the identity range (PML4[0]) is **mutated** by `vmm_space_destroy()`
when zeroing `pdpt[4..15]`. This is kernel-controlled, not user-reachable.

**Finding:** User cannot modify kernel page tables from ring-3. ✓
**Finding:** No user-writable PML4 or PDPT entries for kernel regions. ✓

---

## 6. Vulnerability Table

| ID | Component | Description | Severity |
|----|-----------|-------------|----------|
| V1 | `syscall.c:19` | `SYS_OPEN` path pointer not validated (user range) | HIGH |
| V2 | `syscall.c:21` | `SYS_READ` buf pointer not validated | HIGH |
| V3 | `syscall.c:22` | `SYS_WRITE` buf pointer not validated | HIGH |
| V4 | `syscall.c:23` | `SYS_DRIVER` struct pointer not validated | CRITICAL |
| V5 | `driver_if.c:119` | `req->value` written back to potentially kernel address | CRITICAL |
| V6 | `vmm.c:820` | `vmm_harden_user_space()` is a no-op due to wrong address calc | MEDIUM |
| V7 | `vmm.c:729` | `vmm_map()` accepts USER flag for any VA, including kernel | MEDIUM |
| V8 | `vmm.c:571` | `vmm_space_create_user()` uses physical address as virtual | LOW |

### Classified by impact

| Severity | Count | Action |
|----------|-------|--------|
| CRITICAL | 2 | Address in M3 FASE 2 (copy_from_user, syscall hardening) |
| HIGH | 3 | Address in M3 FASE 2 (user range validation) |
| MEDIUM | 2 | Address in M3 FASE 2 (vmm_map enforcement, harden fix) |
| LOW | 1 | Address in M3 FASE 2 (physmap pointer fix) |

---

## 7. Non-findings (things that are correct)

- Kernel stack switch on INT 0x80: **correct** (TSS.rsp0 used by CPU). ✓
- W^X for kernel sections: enforced by `vmm_protect_kernel_sections()`. ✓
- W^X for user ELF segments: enforced by ELF loader. ✓
- Guard pages: present below kernel stack and user stack. ✓
- PML4[256+] U=0 in user spaces: correct (stripped in vmm_space_create_user). ✓
- NX on kernel data/physmap: enforced. ✓
- Exclusive driver binding: enforced in driver_if.c. ✓

---

*Audit completed on M2_STABLE. No code modified.*
*Next step: FASE 2 — Hardening implementation.*
