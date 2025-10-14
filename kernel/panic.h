#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

// Struttura dei registri salvati durante un'eccezione
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

// Panic - ferma il sistema con un messaggio
void kernel_panic(const char* message, const char* file, uint32_t line);

// Macro per chiamare panic
#define PANIC(msg) kernel_panic(msg, __FILE__, __LINE__)

// Assert: se condizione falsa genera PANIC con messaggio descrittivo
#define ASSERT(cond) do { if (!(cond)) kernel_panic("Assertion failed: " #cond, __FILE__, __LINE__); } while (0)

// Handler generico per eccezioni
void exception_handler(struct registers* regs);

// Nomi delle eccezioni CPU
extern const char* exception_messages[];

#endif