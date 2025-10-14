#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// Struttura di un entry dell'IDT
struct idt_entry {
    uint16_t offset_low;    // Offset bits 0-15
    uint16_t selector;      // Selettore del segmento di codice
    uint8_t  ist;           // Interrupt Stack Table
    uint8_t  type_attr;     // Type e attributi
    uint16_t offset_mid;    // Offset bits 16-31
    uint32_t offset_high;   // Offset bits 32-63
    uint32_t zero;          // Riservato
} __attribute__((packed));

// Struttura del puntatore IDT
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// Funzioni pubbliche
void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags);
void idt_set_gate_ist(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags, uint8_t ist);

// Exception handlers (INT 0-31)
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

// IRQ handlers
extern void isr_timer(void);
extern void isr_keyboard(void);
extern void isr_stub(void);

#endif