BITS 64

section .text

; Funzione per caricare l'IDT
global idt_load
idt_load:
    lidt [rdi]
    ret

; Funzione per caricare il GDT
global gdt_flush
global tss_flush

; void gdt_flush(uint64_t gdt_ptr_addr)
; rdi = indirizzo struttura {limit(16) base(64)}
gdt_flush:
    lgdt [rdi]               ; Carica il nuovo GDT
    mov ax, 0x10             ; Data segment selector (2nd entry)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ; Far jump per aggiornare CS
    push 0x08                ; Code segment selector
    lea rax, [rel .flush_done]
    push rax
    ; Usa retfq per completare il far control transfer con nuovo CS
    retfq
.flush_done:
    ret

; void tss_flush(uint16_t tss_selector)
; rdi = selector (es. 0x28)
tss_flush:
    mov ax, di
    ltr ax                  ; Carica Task Register con selector TSS
    ret

; Macro per creare exception handler senza error code
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push 0              ; Dummy error code
    push %1             ; Interrupt number
    jmp isr_common
%endmacro

; Macro per creare exception handler con error code
%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push %1             ; Interrupt number
    jmp isr_common
%endmacro

; Exception handlers (INT 0-31)
ISR_NOERRCODE 0     ; Division By Zero
ISR_NOERRCODE 1     ; Debug
ISR_NOERRCODE 2     ; Non Maskable Interrupt
ISR_NOERRCODE 3     ; Breakpoint
ISR_NOERRCODE 4     ; Into Detected Overflow
ISR_NOERRCODE 5     ; Out of Bounds
ISR_NOERRCODE 6     ; Invalid Opcode
ISR_NOERRCODE 7     ; No Coprocessor
ISR_ERRCODE   8     ; Double Fault
ISR_NOERRCODE 9     ; Coprocessor Segment Overrun
ISR_ERRCODE   10    ; Bad TSS
ISR_ERRCODE   11    ; Segment Not Present
ISR_ERRCODE   12    ; Stack Fault
ISR_ERRCODE   13    ; General Protection Fault
ISR_ERRCODE   14    ; Page Fault
ISR_NOERRCODE 15    ; Reserved
ISR_NOERRCODE 16    ; Coprocessor Fault
ISR_ERRCODE   17    ; Alignment Check
ISR_NOERRCODE 18    ; Machine Check
ISR_NOERRCODE 19    ; Reserved
ISR_NOERRCODE 20    ; Reserved
ISR_NOERRCODE 21    ; Reserved
ISR_NOERRCODE 22    ; Reserved
ISR_NOERRCODE 23    ; Reserved
ISR_NOERRCODE 24    ; Reserved
ISR_NOERRCODE 25    ; Reserved
ISR_NOERRCODE 26    ; Reserved
ISR_NOERRCODE 27    ; Reserved
ISR_NOERRCODE 28    ; Reserved
ISR_NOERRCODE 29    ; Reserved
ISR_ERRCODE   30    ; Security Exception
ISR_NOERRCODE 31    ; Reserved

; Common ISR handler
extern exception_handler
isr_common:
    ; Salva tutti i registri
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Passa il puntatore alla struttura registers
    mov rdi, rsp
    call exception_handler
    
    ; Ripristina registri
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    ; Rimuovi error code e interrupt number
    add rsp, 16
    
    iretq

; Handler stub generico
global isr_stub
isr_stub:
    iretq

; Handler per il timer (IRQ0 = interrupt 0x20)
global isr_timer
extern timer_handler

isr_timer:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Chiama il gestore C
    call timer_handler
    
    ; Invia EOI (End Of Interrupt) al PIC
    mov al, 0x20
    out 0x20, al
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    iretq

; Handler per la tastiera (IRQ1 = interrupt 0x21)
global isr_keyboard
extern keyboard_handler

isr_keyboard:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Chiama il gestore C
    call keyboard_handler
    
    ; Invia EOI (End Of Interrupt) al PIC
    mov al, 0x20
    out 0x20, al
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    iretq