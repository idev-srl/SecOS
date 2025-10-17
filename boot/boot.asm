; SecOS Kernel - Minimal Long Mode Bootloader
; Copyright (c) 2025 iDev srl
; Author: Luigi De Astis <l.deastis@idev-srl.com>
; SPDX-License-Identifier: MIT
; boot.asm - Minimal Long Mode bootloader
BITS 32

; Multiboot2 minimal header (dedicated section placed at file start)
section .multiboot
align 8
; --- Multiboot2 header (minimal: framebuffer request 1024x768x32) ---
; Tag order:
;  type=5 framebuffer request (size=20) + padding (4)
;  type=0 end

mb2_header_start:
    dd 0xE85250D6            ; magic
    dd 0x0                   ; arch
    dd mb2_header_end - mb2_header_start ; header_length
    dd 0 - (0xE85250D6 + 0x0 + (mb2_header_end - mb2_header_start)) ; checksum
    ; Framebuffer tag (type=5) request (width,height,depth). Size=20 (header 8 + payload 12). Must pad to 8-align next tag.
    dw 5                     ; type
    dw 0                     ; flags
    dd 20                    ; size (does NOT include padding)
    dd 1024                  ; width
    dd 768                   ; height
    dd 32                    ; depth
    dd 0                     ; 4-byte padding to align following tag to 8-byte boundary
    ; End tag
    dw 0                     ; type=0 end
    dw 0                     ; flags
    dd 8                     ; size=8
mb2_header_end:

; Kernel stack
section .bss
align 16
global stack_bottom
global stack_top
stack_bottom:
    resb 16384
stack_top:

; Buffer to copy multiboot structure (up to 8KB)
align 16
global mb2_copy
mb2_copy:
    resb 8192

; Page tables (identity + higher-half mappings prepared later)
align 4096
pml4:
    resb 4096
pdpt:
    resb 4096
pdt:
    resb 4096

section .data
; Saved multiboot parameters
mb_magic: dq 0
mb_info: dq 0

; Resto del codice
section .text
global _start
extern kernel_main

%define DEBUG_PORT 0
%macro PORT_CH 1
%if DEBUG_PORT
    mov al, %1
    out 0xE9, al
%endif
%endmacro

_start:
    cli
    mov esp, stack_top
         ; Salva parametri MULTIBOOT appena entrati
         mov [mb_magic], eax
         mov [mb_info], ebx
         ; Verifica subito magic prima di altre operazioni di dump
        cmp eax, 0x36d76289
        jne .no_mb2_early
        PORT_CH 'M'
        jmp .after_magic_early
    .no_mb2_early:
        PORT_CH 'h'
.after_magic_early:
    PORT_CH 'H'
; (header byte dump removed in cleaned build)
    ; Verifica checksum runtime: somma primi 16 byte deve essere 0 mod 2^32
    mov esi, mb2_header_start
    mov eax, [esi]
    add eax, [esi+8]
    add eax, [esi+12]
    test eax, eax
    jnz .chk_fail
    PORT_CH 'Z'
    jmp .after_chk
.chk_fail:
    PORT_CH '!'
    PORT_CH 'B'
.after_chk:
; (EBX nibble dump removed)
    ; Se magic MB2 (abbiamo emesso 'M' già) prova a copiare struttura info in buffer sicuro
    cmp byte [mb_magic], 0x89        ; check lowest byte of magic 0x36d76289 (quick heuristic)
    jne .skip_copy
    test esi, esi
    jz .skip_copy
    mov edi, esi
    ; Leggi total_size (primi 4 byte) se accessibile
    mov eax, [esi]
    test eax, eax
    jz .skip_copy
    cmp eax, 8192
    ja .skip_copy
    mov ecx, eax
    mov edi, mb2_copy
    mov edx, ecx
    rep movsb
    ; Aggiorna mb_info con nuovo indirizzo buffer
    mov eax, mb2_copy
    mov [mb_info], eax
    PORT_CH 'C'
.skip_copy:
    ; Debug: emetti 'H' se magic = MB2
    cmp eax, 0x36d76289
    jne .no_mb2
    jmp .after_mb2
.no_mb2:
    PORT_CH 'h'
.after_mb2:

    ; Verifica CPUID
    mov ecx, eax
    xor eax, (1 << 21)
    push eax
    popfd
    pushfd
    pop eax
    xor eax, ecx
    jz .no_cpuid
    ; CPUID disponibile
    
    ; Verifica Long Mode
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)
    jz .no_long_mode
    ; Long mode support verificato
    
    ; Setup page tables
    ; Zero out tables
    mov edi, pml4
    mov ecx, 3072
    xor eax, eax
    rep stosd
    
    ; Link tables
    mov eax, pdpt
    or eax, 0x3
    mov [pml4], eax
    
    mov eax, pdt
    or eax, 0x3
    mov [pdpt], eax
    
    ; Mappa 512MB con pagine da 2MB (256 entries)
    ; Flags: Present|Write|PS=2MB => 0x83
    xor ebx, ebx            ; ebx = index
    mov ecx, 256            ; number of 2MB entries
.map_loop:
    mov eax, ebx
    shl eax, 21             ; eax = base phys = index * 2MB
    or eax, 0x83
    mov [pdt + ebx*8], eax
    inc ebx
    cmp ebx, ecx
    jl .map_loop
    ; Identity map 512MB completata
    
    ; Enable PAE
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax
    
    ; Load PML4
    mov eax, pml4
    mov cr3, eax
    ; CR3 caricato
    
    ; Enable long mode + NXE (bit 11 di EFER) per usare NX pages
    mov ecx, 0xC0000080          ; EFER
    rdmsr
    or eax, (1 << 8)             ; LME
    or eax, (1 << 11)            ; NXE
    wrmsr
    ; LME+NXE abilitati
    
    ; Enable paging
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax
    ; Paging abilitato
    
    ; Load GDT
    lgdt [gdt.pointer]
    ; GDT caricata
    jmp 0x08:long_mode

.no_cpuid:
    mov al, 'C'
    jmp error

.no_long_mode:
    mov al, 'L'
    jmp error
error:
    mov dword [0xb8000], 0x4f524f45
    mov byte [0xb8004], al
    jmp error

BITS 64
long_mode:
    ; Entrato in long mode
    ; Setup segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Non azzeriamo più la VGA text memory se useremo il framebuffer
    
    ; Setup stack
    mov rsp, stack_top
    
    ; Get multiboot params
    ; Carica parametri 64-bit per SysV: RDI=magic (zero-extended), RSI=info pointer
    mov eax, dword [mb_magic]   ; magic 32-bit
    mov rdi, rax
    mov eax, dword [mb_info]    ; info low 32-bit
    mov rsi, rax                ; zero-extend
    ; marker before calling kernel_main
    PORT_CH 'I'
    
    ; Call kernel
    call kernel_main
    PORT_CH 'K'
.hang:
    cli
    hlt
section .rodata
align 16
gdt:
    dq 0                                    ; Null
    dq 0x00209A0000000000                  ; Code
    dq 0x0000920000000000                  ; Data
.pointer:
    dw $ - gdt - 1
    dq gdt