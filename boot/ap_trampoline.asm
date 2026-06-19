; SecOS — AP (Application Processor) startup trampoline (M29-2 SMP)
; Copyright (c) 2026 iDev srl
; Author: Luigi De Astis <l.deastis@idev-srl.com>
; SPDX-License-Identifier: MIT
;
; Assembled as a FLAT BINARY with `org 0x8000` and copied to physical 0x8000 at
; runtime. A SIPI starts the AP in 16-bit real mode at CS=0x0800, IP=0 → linear
; 0x8000. We bring it real → protected → long mode using the BSP's kernel CR3
; (which maps both the low identity 0–512 MiB, where this code lives, and the
; high half, where ap_entry runs), then jump to the C entry on a per-AP stack.
;
; The BSP fills a parameter block at the fixed physical addresses below before
; sending the SIPI (one AP at a time, so the shared page is race-free):
;   0x8F00 qword  CR3 (kernel PML4 physical)
;   0x8F08 qword  per-AP kernel stack top (high-half VA)
;   0x8F10 qword  ap_entry C function (high-half VA)

%define AP_BASE        0x8000
%define AP_PARAM_CR3   0x8F00
%define AP_PARAM_STACK 0x8F08
%define AP_PARAM_ENTRY 0x8F10

[bits 16]
[org AP_BASE]
global ap_trampoline_start
ap_trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    lgdt [ap_gdt32_ptr]              ; 32-bit GDT (flat, base 0)
    mov eax, cr0
    or eax, 1                        ; PE
    mov cr0, eax
    jmp 0x08:ap_pmode               ; far jump to 32-bit code segment

[bits 32]
ap_pmode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov eax, cr4
    or eax, (1 << 5)                 ; PAE
    mov cr4, eax

    mov eax, [AP_PARAM_CR3]          ; kernel PML4 (low 32 bits suffice <4 GiB)
    mov cr3, eax

    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or eax, (1 << 8)                 ; LME
    or eax, (1 << 11)                ; NXE
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)                ; PG
    mov cr0, eax

    lgdt [ap_gdt64_ptr]             ; 64-bit GDT
    jmp 0x08:ap_long

[bits 64]
ap_long:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, [AP_PARAM_STACK]        ; per-AP kernel stack (high-half VA, mapped)
    xor rbp, rbp
    mov rax, [AP_PARAM_ENTRY]
    call rax                         ; ap_entry() — never returns
.hang:
    cli
    hlt
    jmp .hang

; --- 32-bit flat GDT (null, code32, data32) ---
align 16
ap_gdt32:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF           ; code32: base0 lim4G G,D,RX
    dq 0x00CF92000000FFFF           ; data32
ap_gdt32_ptr:
    dw ap_gdt32_ptr - ap_gdt32 - 1
    dd ap_gdt32

; --- 64-bit GDT (null, code64, data64) ---
align 16
ap_gdt64:
    dq 0x0000000000000000
    dq 0x00209A0000000000           ; code64: L=1
    dq 0x0000920000000000           ; data64
ap_gdt64_ptr:
    dw ap_gdt64_ptr - ap_gdt64 - 1
    dd ap_gdt64

global ap_trampoline_end
ap_trampoline_end:
