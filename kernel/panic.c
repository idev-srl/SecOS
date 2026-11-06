/*
 * SecOS Kernel - Panic & Exception Handling
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "debugcon.h"
#include "panic.h"
#include "terminal.h"
#include "trapframe.h"
#include "sched.h"
#include "process.h"
#include "vmm.h"
#include "config.h"
#include "fb_console.h"
#include "kverbose.h"
#include "../mm/user_copy.h"
#include <stdint.h>

// [M45] KASLR: a kernel-image address as it is in kernel.bin (runtime VA minus
// the slide), so logged addresses resolve with addr2line/nm and do not reveal
// the slide. Other addresses are returned unchanged.
static uint64_t link_addr(uint64_t a) {
    uint64_t base = kernel_vma_base();
    if (a >= base && a - base < (512ULL << 20)) return a - g_kaslr_slide;
    return a;
}

/* [M39] Debug breadcrumb: drivers doing long in-kernel bring-up (ath9k) write
 * their last progress line here. The exception handler clears the screen, so we
 * reprint it inside the panic dump — it's how we localize a fault that happens
 * deep inside a ported driver without ever returning to the shell. */
char g_dbg_breadcrumb[200] = {0};

/* [M36] Stack-smashing protection. -fstack-protector-strong inserts a canary on
 * function entry and calls __stack_chk_fail() if it is corrupted on return. The
 * guard is a fixed non-zero value (KASLR would randomize it per boot). A failure
 * means a kernel stack buffer overflowed — unrecoverable, halt loudly. */
uintptr_t __stack_chk_guard = 0x9d8f2b1c7e6a4053UL;
__attribute__((noreturn)) void __stack_chk_fail(void) {
    debugcon_writestring("[M36] *** stack smashing detected (canary) — halting ***\n");
    terminal_writestring("\n*** STACK SMASHING DETECTED — kernel halted ***\n");
    for (;;) __asm__ volatile("cli; hlt");
}

// CPU exception names (INT 0-31)
const char* exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

/* Set once the kernel has committed to a ring-0 panic (kernel_panic or a fatal
 * ring-0 exception). Painting the panic screen touches the framebuffer and the
 * terminal; if that itself faults (e.g. the fault was caused by corrupted page
 * tables), re-entering the panic path would recurse — each nested #PF repainted
 * and faulted again, flooding debugcon with hundreds of thousands of lines until
 * the stack ran out. A fault while this is set halts at once with one line. */
static volatile int g_in_kernel_panic = 0;

/* Enter the panic path. Returns normally the first time; a nested entry (a fault
 * raised while already panicking) reports once on debugcon and halts for good.
 * Also switches to the kernel address space: the faulting context's CR3 may be
 * the very thing that is corrupted (a process PML4 with its physmap entry wiped),
 * while the kernel PML4 still maps the physmap + framebuffer, so the screen can
 * be painted. Kernel stacks/IST live in the shared high half, so this is safe. */
static void panic_enter(const char* what) {
    __asm__ volatile("cli");
    if (g_in_kernel_panic) {
        debugcon_writestring("[PANIC] nested fault inside the panic path (");
        debugcon_writestring(what);
        debugcon_writestring(") -> halting\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    g_in_kernel_panic = 1;
    vmm_switch_space(vmm_get_kernel_space());
#if ENABLE_FB
    // terminal_initialize() would kfree() the console back buffer; the heap may
    // be corrupted (the reason we are here), so detach it without heap access.
    fb_console_panic_detach_dbuf();
#endif
}

// Convert number to hexadecimal string
static void print_hex64(uint64_t value) {
    char hex_chars[] = "0123456789ABCDEF";
    terminal_writestring("0x");
    
    for (int i = 60; i >= 0; i -= 4) {
        char c = hex_chars[(value >> i) & 0xF];
        terminal_putchar(c);
    }
}

// Ferma il sistema con un messaggio di panic
void kernel_panic(const char* message, const char* file, uint32_t line) {
    debugcon_writestring("[PANIC] "); debugcon_writestring(message); debugcon_writestring("\n");
    panic_enter("kernel_panic");   // cli + recursion guard + kernel CR3
    
    // Schermo rosso
    terminal_initialize();
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    
    // Banner
    terminal_writestring("\n");
    terminal_writestring("================================================================================\n");
    terminal_writestring("                            KERNEL PANIC                                        \n");
    terminal_writestring("================================================================================\n");
    terminal_writestring("\n");
    
    // Messaggio
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_RED));
    terminal_writestring("Message: ");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    terminal_writestring(message);
    terminal_writestring("\n\n");
    
    // Posizione nel codice
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_RED));
    terminal_writestring("File: ");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    terminal_writestring(file);
    terminal_writestring("\n");
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_RED));
    terminal_writestring("Line: ");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    
    // Stampa numero linea
    char buf[16];
    if (line == 0) {
        terminal_writestring("0");
    } else {
        int i = 0;
        uint32_t n = line;
        while (n > 0) {
            buf[i++] = '0' + (n % 10);
            n /= 10;
        }
        while (i > 0) {
            terminal_putchar(buf[--i]);
        }
    }
    terminal_writestring("\n\n");
    
    // Istruzioni
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_RED));
    terminal_writestring("Il sistema e' stato arrestato per prevenire danni.\n");
    terminal_writestring("Premere il tasto reset per riavviare.\n");
    terminal_writestring("\n");
    terminal_writestring("================================================================================\n");
    
    // Ferma il sistema
    while (1) {
        __asm__ volatile("hlt");
    }
}

// Handler per eccezioni CPU (unica definizione corretta)
// Map a CPU exception vector to the POSIX signal a ring-3 fault terminates the
// process with (Linux conventions), so wait() reports e.g. "Segmentation fault"
// for a #PF instead of a raw vector number.
static int exc_to_signal(uint64_t vec) {
    switch (vec) {
    case 0:  return 8;   /* #DE divide error        -> SIGFPE  */
    case 1:  return 5;   /* #DB debug               -> SIGTRAP */
    case 3:  return 5;   /* #BP breakpoint          -> SIGTRAP */
    case 4:  return 11;  /* #OF overflow (into)     -> SIGSEGV */
    case 5:  return 11;  /* #BR bound range         -> SIGSEGV */
    case 6:  return 4;   /* #UD invalid opcode      -> SIGILL  */
    case 7:  return 8;   /* #NM device n/a          -> SIGFPE  */
    case 10: return 11;  /* #TS invalid TSS         -> SIGSEGV */
    case 11: return 7;   /* #NP segment not present -> SIGBUS  */
    case 12: return 7;   /* #SS stack-segment fault -> SIGBUS  */
    case 13: return 11;  /* #GP general protection  -> SIGSEGV */
    case 14: return 11;  /* #PF page fault          -> SIGSEGV */
    case 16: return 8;   /* #MF x87 FP error        -> SIGFPE  */
    case 17: return 7;   /* #AC alignment check     -> SIGBUS  */
    case 19: return 8;   /* #XM SIMD FP exception   -> SIGFPE  */
    default: return 11;  /* anything else           -> SIGSEGV */
    }
}

void exception_handler(struct registers* regs) {
    __asm__ volatile("cli");

    // [M6] Save trapframe snapshot into current process (struct registers
    // and trapframe_t share the same binary layout). Only for a ring-3 fault:
    // a ring-0 fault (e.g. a syscall touching a user buffer) carries kernel
    // state, which must never overwrite the process's saved user context.
    if ((regs->cs & 3) == 3) {
        process_t* cur = sched_get_current();
        if (cur && cur->tf) {
            const uint8_t* s = (const uint8_t*)regs;
            uint8_t* d = (uint8_t*)cur->tf;
            for (int i = 0; i < (int)sizeof(trapframe_t); i++) d[i] = s[i];
        }
    }

    uint64_t int_no = regs->int_no;
    uint64_t err_code = regs->err_code;

    if (int_no == 14) { // Page Fault
        uint64_t cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        // 1. Demand paging / COW first — also for ring-0 faults on user
        //    addresses (a not-yet-faulted syscall buffer, M14).
        if (vmm_try_handle_page_fault(cr2, err_code) == 0) return;
        // 2. Exception fixup: a ring-0 fault on a USER address raised by a
        //    registered user-access instruction (copy_from/to_user,
        //    strncpy_from_user) resumes at its fixup, which returns -EFAULT to
        //    the syscall. Must run before panic_enter(). Any other ring-0 fault
        //    on a user address is a kernel bug and still panics below.
        if ((regs->cs & 3) != 3 && cr2 < USER_ADDR_MAX) {
            uint64_t fix = uaccess_fixup_lookup(regs->rip);
            if (fix) {
                if (g_kverbose) {
                    debugcon_writestring("[UACCESS] -EFAULT fixup addr=");
                    debugcon_print_hex(cr2);
                    debugcon_writestring(" rip="); debugcon_print_hex(link_addr(regs->rip));
                    debugcon_writestring("\n");
                }
                regs->rip = fix;
                return;
            }
        }
        vmm_report_page_fault(cr2, err_code);
        // A genuine fault — fall through to the kill/panic logic below.
    }

    if (int_no < 32) {
        // Headless crash diagnosis: surface the fault on debugcon (the smoke
        // harness captures port 0xE9; the framebuffer is not visible there).
        int from_user = ((regs->cs & 3) == 3);
        { uint64_t _cr2; __asm__ volatile("mov %%cr2, %0":"=r"(_cr2));
          debugcon_writestring("[EXC] int="); debugcon_print_hex(int_no);
          debugcon_writestring(" err="); debugcon_print_hex(err_code);
          debugcon_writestring(" rip="); debugcon_print_hex(regs->rip);
          if (!from_user) { debugcon_writestring(" lrip="); debugcon_print_hex(link_addr(regs->rip)); }
          debugcon_writestring(" cr2="); debugcon_print_hex(_cr2);
          debugcon_writestring(" cs="); debugcon_print_hex(regs->cs);
          debugcon_writestring(" rsp="); debugcon_print_hex(regs->rsp);
          debugcon_writestring(from_user ? " ring3\n" : " ring0\n"); }

        // [M15] A ring-3 fault is the offending PROCESS's bug, not the kernel's:
        // terminate that process and return to the scheduler instead of halting
        // the whole machine. A ring-0 fault is a real kernel bug -> panic below.
        if (from_user) {
            process_t* cur = sched_get_current();
            if (cur) {
                debugcon_writestring("[EXC] ring3 fault -> kill pid=");
                debugcon_print_hex(cur->pid); debugcon_writestring("\n");
                extern void sched_kill_current(int sig) __attribute__((noreturn));
                sched_kill_current(exc_to_signal(int_no)); // NORETURN: switches to next task
            }
        }

        // Kernel-mode fault (or no current process): unrecoverable -> panic/halt.
        panic_enter("exception");      // recursion guard + kernel CR3 before painting
        terminal_initialize();
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
        terminal_writestring("\n=== EXCEPTION ===\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_YELLOW, VGA_COLOR_RED));
        terminal_writestring("Tipo: ");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
        terminal_writestring(exception_messages[int_no]);
        terminal_writestring("\nCodice errore: 0x");
        char hex_chars[] = "0123456789ABCDEF";
        for (int i = 60; i >= 0; i -= 4) terminal_putchar(hex_chars[(err_code >> i) & 0xF]);
        terminal_writestring("\nRIP: "); print_hex64(regs->rip);
        terminal_writestring(" (kernel.bin: "); print_hex64(link_addr(regs->rip)); terminal_writestring(")");
        terminal_writestring("\nRSP: "); print_hex64(regs->rsp);
        terminal_writestring("\nCS: "); print_hex64(regs->cs);
        terminal_writestring("\nRFLAGS: "); print_hex64(regs->rflags);
        { uint64_t _cr2; __asm__ volatile("mov %%cr2, %0":"=r"(_cr2));
          terminal_writestring("\nCR2: "); print_hex64(_cr2); }
        if (g_dbg_breadcrumb[0]) { terminal_writestring("\nLast: "); terminal_writestring(g_dbg_breadcrumb); }
        terminal_writestring("\n\nSistema bloccato.\n");
        debugcon_writestring("[PANIC] ring-0 exception: system halted\n");
        while (1) { __asm__ volatile("cli; hlt"); }
    }
}