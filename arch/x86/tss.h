#ifndef TSS_H
#define TSS_H

#include <stdint.h>

// TSS (Task State Segment) per x86-64
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;        // Stack pointer per ring 0
    uint64_t rsp1;        // Stack pointer per ring 1
    uint64_t rsp2;        // Stack pointer per ring 2
    uint64_t reserved1;
    uint64_t ist1;        // Interrupt Stack Table 1
    uint64_t ist2;        // Interrupt Stack Table 2
    uint64_t ist3;        // Interrupt Stack Table 3
    uint64_t ist4;        // Interrupt Stack Table 4
    uint64_t ist5;        // Interrupt Stack Table 5
    uint64_t ist6;        // Interrupt Stack Table 6
    uint64_t ist7;        // Interrupt Stack Table 7
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

// Entry del GDT
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

// TSS entry nel GDT (64-bit, occupa 2 slot)
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed)) gdt_tss_entry_t;

// Pointer al GDT
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

// Funzioni pubbliche
void tss_init(void);
void tss_set_kernel_stack(uint64_t stack);

#endif