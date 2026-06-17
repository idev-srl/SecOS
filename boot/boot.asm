; SecOS Kernel - Higher-half Long Mode Bootloader
; Copyright (c) 2025 iDev srl
; Author: Luigi De Astis <l.deastis@idev-srl.com>
; SPDX-License-Identifier: MIT
;
; The kernel runs at the canonical high half KERNEL_VMA = 0xFFFFFFFF80000000.
; This file's low .boot.* sections run at their physical load address (identity)
; to perform the 32-bit Multiboot2 -> long-mode transition, build page tables
; that map BOTH the low identity (0-512MB) and the high half, then jump to the
; high-half kernel entry. The UEFI path enters at _uefi_start (already 64-bit;
; the external loader has mapped the high half) and joins the same flow.
BITS 32

KERNEL_VMA equ 0xFFFFFFFF80000000

; --- Multiboot2 header (collected first into .boot by the linker) ---
section .multiboot
align 8
mb2_header_start:
    dd 0xE85250D6            ; magic
    dd 0x0                   ; arch
    dd mb2_header_end - mb2_header_start ; header_length
    dd 0 - (0xE85250D6 + 0x0 + (mb2_header_end - mb2_header_start)) ; checksum
    dw 5                     ; type=5 framebuffer request
    dw 0                     ; flags
    dd 20                    ; size
    dd 1024                  ; width
    dd 768                   ; height
    dd 32                    ; depth
    dd 0                     ; padding to 8-align
    dw 0                     ; end tag
    dw 0
    dd 8
mb2_header_end:

; --- Low boot BSS: page tables, boot stack, MB info copy buffer ---
section .boot.bss nobits alloc noexec write align=4096
align 4096
pml4:     resb 4096
pdpt_low: resb 4096          ; PML4[0]   -> identity 0-512MB
pdt:      resb 4096          ; shared 512MB page-directory (2MB pages)
pdpt_hi:  resb 4096          ; PML4[511] -> high half (0xFFFFFFFF80000000)
align 16
boot_stack_bottom:
    resb 4096
boot_stack_top:
align 16
mb2_copy:
    resb 8192

; --- Low boot data: saved MB params + boot GDT (used by both transitions) ---
section .boot.data progbits alloc noexec write align=16
mb_magic: dq 0
mb_info:  dq 0
align 16
gdt:
    dq 0                                    ; Null
    dq 0x00209A0000000000                   ; Code (64-bit)
    dq 0x0000920000000000                   ; Data
gdt_pointer:
    dw gdt_pointer - gdt - 1
    dq gdt

; --- Low boot text: 32-bit setup + 64-bit low trampoline ---
section .boot.text progbits alloc exec nowrite align=16
global _start
extern kernel_main

_start:
    cli
    mov esp, boot_stack_top
    mov [mb_magic], eax
    mov [mb_info], ebx

    ; Copy Multiboot2 info into a safe buffer if it looks sane (<=8KB).
    cmp eax, 0x36d76289
    jne .skip_copy
    mov esi, ebx
    test esi, esi
    jz .skip_copy
    mov eax, [esi]              ; total_size
    test eax, eax
    jz .skip_copy
    cmp eax, 8192
    ja .skip_copy
    mov ecx, eax
    mov edi, mb2_copy
    rep movsb
    mov dword [mb_info], mb2_copy
.skip_copy:

    ; CPUID availability
    pushfd
    pop eax
    mov ecx, eax
    xor eax, (1 << 21)
    push eax
    popfd
    pushfd
    pop eax
    xor eax, ecx
    jz .no_cpuid

    ; Long mode availability
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)
    jz .no_long_mode

    ; Zero the page tables (4 * 4096 = 16384 bytes = 4096 dwords)
    mov edi, pml4
    mov ecx, 4096
    xor eax, eax
    rep stosd

    ; PML4[0] -> pdpt_low ; pdpt_low[0] -> pdt   (low identity)
    mov eax, pdpt_low
    or eax, 0x3
    mov [pml4], eax
    mov eax, pdt
    or eax, 0x3
    mov [pdpt_low], eax

    ; PML4[511] -> pdpt_hi ; pdpt_hi[510] -> pdt  (high half)
    ; 0xFFFFFFFF80000000: PML4 idx 511 (off 0xFF8), PDPT idx 510 (off 0xFF0).
    mov eax, pdpt_hi
    or eax, 0x3
    mov [pml4 + 511*8], eax
    mov eax, pdt
    or eax, 0x3
    mov [pdpt_hi + 510*8], eax

    ; Fill the shared 512MB page-directory with 256 * 2MB pages.
    xor ebx, ebx
.map_loop:
    mov eax, ebx
    shl eax, 21                 ; phys = idx * 2MB
    or eax, 0x83                ; Present|Write|PS
    mov [pdt + ebx*8], eax
    inc ebx
    cmp ebx, 256
    jl .map_loop

    ; Enable PAE
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; Load PML4
    mov eax, pml4
    mov cr3, eax

    ; Enable Long Mode + NX in EFER
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)            ; LME
    or eax, (1 << 11)           ; NXE
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    ; Load boot GDT and far-jump into 64-bit (low identity) trampoline
    lgdt [gdt_pointer]
    jmp 0x08:long_mode_low

.no_cpuid:
    mov al, 'C'
    jmp .error
.no_long_mode:
    mov al, 'L'
.error:
    mov dword [0xb8000], 0x4f524f45
    mov byte [0xb8004], al
    jmp .error

; 64-bit trampoline still executing at the low identity address. Set up the
; SysV args for kernel_main, then jump to the high-half entry (indirect, so the
; full 64-bit target address is reachable).
BITS 64
long_mode_low:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov eax, dword [mb_magic]
    mov rdi, rax               ; arg0 = multiboot magic
    mov eax, dword [mb_info]
    mov rsi, rax               ; arg1 = multiboot info pointer
    mov rax, long_mode_high
    jmp rax

; --- High-half kernel entry ---
section .text
BITS 64
long_mode_high:
    lea rsp, [rel stack_top]   ; switch to the high-half boot stack
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang

; ── UEFI entry point ─────────────────────────────────────────────────────────
; Entered in 64-bit long mode by the external UEFI loader, which has already
; mapped the kernel high half and the low identity. RDI=0 (magic), RSI=&bootinfo.
global _uefi_start
_uefi_start:
    ; Load the boot GDT (low identity, still mapped) via an absolute pointer:
    ; a RIP-relative lgdt from the high half cannot reach the low descriptor.
    mov rax, gdt_pointer
    lgdt [rax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    lea rsp, [rel stack_top]
    call kernel_main
    cli
.uhang:
    hlt
    jmp .uhang

; High-half boot stack (used by both entries until the kernel switches stacks)
section .bss
align 16
global stack_bottom
global stack_top
stack_bottom:
    resb 16384
stack_top:
