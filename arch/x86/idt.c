#include "idt.h"

#define IDT_ENTRIES 256

// Array dell'IDT
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

// Funzione assembly per caricare l'IDT
extern void idt_load(uint64_t);

// Output su porta I/O
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

// Input da porta I/O
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Imposta un gate nell'IDT
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags) {
    idt[num].offset_low = handler & 0xFFFF;
    idt[num].offset_mid = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].selector = selector;
    idt[num].ist = 0;  // Default: non usa IST
    idt[num].type_attr = flags;
    idt[num].zero = 0;
}

// Imposta un gate con IST specifico
void idt_set_gate_ist(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags, uint8_t ist) {
    idt_set_gate(num, handler, selector, flags);
    idt[num].ist = ist;  // Imposta IST
}

// Rimappa il PIC (Programmable Interrupt Controller)
static void pic_remap(void) {
    // Salva le maschere
    uint8_t a1 = inb(0x21);
    uint8_t a2 = inb(0xA1);
    
    // Inizializza PIC master e slave
    outb(0x20, 0x11);  // ICW1: inizializza
    outb(0xA0, 0x11);
    
    outb(0x21, 0x20);  // ICW2: offset IRQ master -> 0x20-0x27
    outb(0xA1, 0x28);  // ICW2: offset IRQ slave -> 0x28-0x2F
    
    outb(0x21, 0x04);  // ICW3: master ha slave su IRQ2
    outb(0xA1, 0x02);  // ICW3: slave su IRQ2 del master
    
    outb(0x21, 0x01);  // ICW4: modalità 8086
    outb(0xA1, 0x01);
    
    // Ripristina le maschere
    outb(0x21, a1);
    outb(0xA1, a2);
}

// Inizializza l'IDT
void idt_init(void) {
    // Imposta il puntatore IDT
    idtp.limit = (sizeof(struct idt_entry) * IDT_ENTRIES) - 1;
    idtp.base = (uint64_t)&idt;
    
    // Azzera l'IDT
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, (uint64_t)isr_stub, 0x08, 0x8E);
    }
    
    // Registra exception handlers (INT 0-31)
    idt_set_gate(0, (uint64_t)isr0, 0x08, 0x8E);
    idt_set_gate(1, (uint64_t)isr1, 0x08, 0x8E);
    idt_set_gate(2, (uint64_t)isr2, 0x08, 0x8E);
    idt_set_gate(3, (uint64_t)isr3, 0x08, 0x8E);
    idt_set_gate(4, (uint64_t)isr4, 0x08, 0x8E);
    idt_set_gate(5, (uint64_t)isr5, 0x08, 0x8E);
    idt_set_gate(6, (uint64_t)isr6, 0x08, 0x8E);
    idt_set_gate(7, (uint64_t)isr7, 0x08, 0x8E);
    idt_set_gate_ist(8, (uint64_t)isr8, 0x08, 0x8E, 1);    // Double Fault con IST1
    idt_set_gate(9, (uint64_t)isr9, 0x08, 0x8E);
    idt_set_gate(10, (uint64_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint64_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint64_t)isr12, 0x08, 0x8E);
    idt_set_gate_ist(13, (uint64_t)isr13, 0x08, 0x8E, 3);  // GPF con IST3
    idt_set_gate_ist(14, (uint64_t)isr14, 0x08, 0x8E, 2);  // Page Fault con IST2
    idt_set_gate(15, (uint64_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint64_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint64_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint64_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint64_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint64_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint64_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint64_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint64_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint64_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint64_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint64_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint64_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint64_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint64_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint64_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint64_t)isr31, 0x08, 0x8E);
    
    // Rimappa il PIC
    pic_remap();
    
    // Imposta handler timer (IRQ0 = interrupt 0x20)
    idt_set_gate(0x20, (uint64_t)isr_timer, 0x08, 0x8E);
    
    // Imposta handler tastiera (IRQ1 = interrupt 0x21)
    idt_set_gate(0x21, (uint64_t)isr_keyboard, 0x08, 0x8E);

    // Syscall gate INT 0x80 (trap gate, present, DPL=3 -> 0xEE: 1110 1110)
    idt_set_gate(0x80, (uint64_t)syscall_entry, 0x08, 0xEE);
    
    // Carica l'IDT
    idt_load((uint64_t)&idtp);
    
    // Abilita IRQ0 (timer) e IRQ1 (tastiera) sul PIC
    outb(0x21, 0xFC);  // 0xFC = 11111100 - abilita IRQ0 e IRQ1
    outb(0xA1, 0xFF);  // Disabilita tutte le IRQ slave
    
    // Abilita gli interrupt
    __asm__ volatile ("sti");
}