/* uefi/crt0.s — Minimal UEFI CRT0 for SECoS (ms_abi efi_main bridge)
 *
 * UEFI firmware calls _start(RCX=ImageHandle, RDX=SystemTable) — Windows ABI.
 * gnu-efi _relocate expects (RDI=ImageBase, RSI=_DYNAMIC) — SysV ABI.
 * Our efi_main is __attribute__((ms_abi)) so expects (RCX=ImageHandle, RDX=SystemTable).
 *
 * The stock crt0-efi-x86_64.o passes args to efi_main in RDI/RSI (SysV), which
 * is wrong for an ms_abi function.  This replacement saves the UEFI args across
 * the _relocate call and re-passes them via RCX/RDX.
 *
 * Copyright (c) 2025 iDev srl
 * SPDX-License-Identifier: MIT
 */

    .section .text
    .global  _start

/* Write a single byte to COM1 (0x3F8) — used for raw serial probe */
.macro serial_byte val
    mov     $0x3F8, %dx
    mov     $\val, %al
    out     %al, %dx
.endm

_start:
    /* On UEFI entry: RCX=ImageHandle, RDX=SystemTable (Windows ABI).
     * RSP = 16K-8 (return address was pushed; stack was 16-aligned before call). */

    push    %rbp                    /* RSP = 16K-16, 16-aligned */
    push    %r14                    /* RSP = 16K-24, 8-mod-16   */
    push    %r15                    /* RSP = 16K-32, 16-aligned */

    /* Stash UEFI args FIRST — serial probe clobbers %dx (low 16 bits of RDX) */
    mov     %rcx, %r14              /* r14 = ImageHandle  */
    mov     %rdx, %r15              /* r15 = SystemTable  */

    /* Raw serial probe: 'S' 'T' 'R' = "_STaRT" indicator on COM1 */
    serial_byte 0x53     /* 'S' */
    serial_byte 0x54     /* 'T' */
    serial_byte 0x52     /* 'R' */

    /* Call _relocate(ImageBase, _DYNAMIC) — SysV ABI.
     * RSP is 16-aligned here, satisfying SysV stack-alignment rule. */
    lea     ImageBase(%rip), %rdi   /* rdi = runtime load base (ImageBase = 0 in ELF) */
    lea     _DYNAMIC(%rip), %rsi    /* rsi = address of .dynamic section              */
    call    _relocate               /* return value ignored (EFI_SUCCESS or nothing to do) */

    /* Call efi_main(ImageHandle, SystemTable) — ms_abi (Windows calling convention).
     * Allocate 32-byte shadow/home space as required by Windows ABI. */
    sub     $32, %rsp               /* RSP = 16K-64, 16-aligned (shadow space)  */
    mov     %r14, %rcx              /* rcx = ImageHandle  */
    mov     %r15, %rdx              /* rdx = SystemTable  */
    call    efi_main                /* returns EFI_STATUS in RAX                */
    add     $32, %rsp               /* restore RSP        */

    /* Return EFI_STATUS (RAX) to UEFI firmware */
    pop     %r15
    pop     %r14
    pop     %rbp
    ret
