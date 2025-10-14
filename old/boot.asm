; boot.asm - Bootloader minimalista per Long Mode
BITS 32

; Multiboot header
section .multiboot
align 4
    dd 0x1BADB002              ; Magic
    dd 0x00                    ; Flags
    dd -(0x1BADB002 + 0x00)    ; Checksum

; Stack
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

; Page tables
align 4096
pml4:
    resb 4096
pdpt:
    resb 4096
pdt:
    resb 4096

section .data
; Salva parametri multiboot
mb_magic: dd 0
mb_info: dd 0

section .text
global _start
extern kernel_main

_start:
    cli
    mov esp, stack_top
    
    ; Salva parametri
    mov [mb_magic], eax
    mov [mb_info], ebx
    
    ; Verifica CPUID
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
    
    ; Verifica Long Mode
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)
    jz .no_long_mode
    
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
    
    ; Mappa 16MB con pagine da 2MB (8 entries)
    ; Flags: Present (1) | Write (2) | Page Size 2MB (bit 7) => 0x83
    ; Base fisica deve essere allineata a 2MB
    mov eax, 0x00000083      ; 0 - 2MB
    mov [pdt + 0*8], eax
    mov eax, 0x00200083      ; 2MB - 4MB
    mov [pdt + 1*8], eax
    mov eax, 0x00400083      ; 4MB - 6MB
    mov [pdt + 2*8], eax
    mov eax, 0x00600083      ; 6MB - 8MB
    mov [pdt + 3*8], eax
    mov eax, 0x00800083      ; 8MB - 10MB
    mov [pdt + 4*8], eax
    mov eax, 0x00A00083      ; 10MB - 12MB
    mov [pdt + 5*8], eax
    mov eax, 0x00C00083      ; 12MB - 14MB
    mov [pdt + 6*8], eax
    mov eax, 0x00E00083      ; 14MB - 16MB
    mov [pdt + 7*8], eax
    
    ; Enable PAE
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax
    
    ; Load PML4
    mov eax, pml4
    mov cr3, eax
    
    ; Enable long mode + NXE (bit 11 di EFER) per usare NX pages
    mov ecx, 0xC0000080          ; EFER
    rdmsr
    or eax, (1 << 8)             ; LME
    or eax, (1 << 11)            ; NXE
    wrmsr
    
    ; Enable paging
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax
    
    ; Load GDT
    lgdt [gdt.pointer]
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
    hlt
    jmp error

BITS 64
long_mode:
    ; Setup segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Clear the screen - write directly
    mov rax, 0x0F200F200F200F20
    mov rdi, 0xB8000
    mov rcx, 500
    rep stosq
    
    ; Setup stack
    mov rsp, stack_top
    
    ; Get multiboot params
    mov edi, [mb_magic]
    mov esi, [mb_info]
    
    ; Call kernel
    call kernel_main
    
.hang:
    cli
    hlt
    jmp .hang

section .rodata
align 16
gdt:
    dq 0                                    ; Null
    dq 0x00209A0000000000                  ; Code
    dq 0x0000920000000000                  ; Data
.pointer:
    dw $ - gdt - 1
    dq gdt