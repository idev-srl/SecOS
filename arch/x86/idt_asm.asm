; SecOS Kernel - IDT & GDT Assembly Helpers
; Copyright (c) 2025 iDev srl
; Author: Luigi De Astis <l.deastis@idev-srl.com>
; SPDX-License-Identifier: MIT
BITS 64

section .text

; Load IDT descriptor pointed by rdi
global idt_load
idt_load:
    lidt [rdi]
    ret

; Load GDT & update segment registers
global gdt_flush
global tss_flush

; void gdt_flush(uint64_t gdt_ptr_addr)
; rdi = pointer to packed {limit(16) base(64)} structure
gdt_flush:
    lgdt [rdi]               ; Load new GDT
    mov ax, 0x10             ; Data segment selector (2nd entry)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ; Far jump to update CS
    push 0x08                ; Code segment selector
    lea rax, [rel .flush_done]
    push rax
    ; Use retfq to complete far control transfer with new CS
    retfq
.flush_done:
    ret

; void tss_flush(uint16_t tss_selector)
; rdi = selector (e.g. 0x28)
tss_flush:
    mov ax, di
    ltr ax                  ; Load Task Register with TSS selector
    ret

; Macro for exception handler without error code
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push 0              ; Dummy error code
    push %1             ; Interrupt number
    jmp isr_common
%endmacro

; Macro for exception handler with error code
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
    ; Build a canonical trapframe_t so the handler can preempt (M8).
    push 0              ; err_code (dummy)
    push 0x20           ; int_no (timer vector)
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

    ; rdi = &trapframe (points at r15).  On a preemptive switch the C handler
    ; sends EOI itself and does NOT return (it iretq's into the next task);
    ; otherwise it returns here and we EOI + iretq into the same task.
    mov rdi, rsp
    call timer_handler

    ; Send EOI (End Of Interrupt) to the PIC
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

    add rsp, 16         ; drop int_no + err_code
    iretq

; Keyboard handler (IRQ1 = interrupt 0x21)
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

; ============================================================================
; MSI/MSI-X interrupt stubs — IMPLEMENTED BUT NOT YET TESTED / not enabled by
; default (the kernel is polled). These vectors (0x40 xHCI, 0x41 NVMe) are only
; wired into the IDT when the gated *_USE_IRQ driver paths register them; the
; default build never installs them and never fires them. They acknowledge via
; the LAPIC (lapic_eoi inside the C handler), NOT the legacy PIC.
; ============================================================================
global isr_xhci
extern xhci_irq_handler
isr_xhci:
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
    call xhci_irq_handler        ; drains/acks; LAPIC EOI done in C
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

global isr_nvme
extern nvme_irq_handler
isr_nvme:
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
    call nvme_irq_handler        ; drains/acks; LAPIC EOI done in C
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

; [M24] NIC MSI-X (vector 0x42). NAPI-style: the C handler acks the NIC, drains
; the RX ring via dev->poll, runs protocol timers, and EOIs the LAPIC. Only wired
; when net_request_irq() picks MSI-X (gated by NET_USE_MSIX); polled by default.
global isr_net
extern net_irq_handler
isr_net:
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
    call net_irq_handler         ; acks NIC, drains ring; LAPIC EOI done in C
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

; ---- M2: Stack-switch trampoline ----
; void trampoline_switch_stack(uint64_t new_rsp, void (*fn)(void));
; rdi = new_rsp  (System V AMD64: first integer arg)
; rsi = fn       (System V AMD64: second integer arg)
;
; Switches RSP to new_rsp, resets RBP to 0 (new frame base),
; then tail-calls fn.  fn must not return.
; NOTE(M2-B): interrupts are disabled at this point (idt_init not yet called).
global trampoline_switch_stack
trampoline_switch_stack:
    mov  rsp, rdi   ; switch to new guarded kernel stack
    xor  rbp, rbp   ; mark bottom of new call chain
    jmp  rsi        ; tail-call kernel_main_phase2 (no return)
