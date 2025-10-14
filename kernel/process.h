#ifndef PROCESS_H
#define PROCESS_H
#include <stdint.h>
#include <stddef.h>
#include "vmm.h"
#include "../mm/elf.h" // per ELF_OK

typedef struct process {
    uint32_t pid;
    vmm_space_t* space;
    uint64_t entry;
    uint64_t stack_top;
    uint64_t kstack_top; // kernel stack top (per trap/switch futuro)
    enum { PROC_NEW, PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_ZOMBIE } state;
    struct regs_snapshot {
        uint64_t rip, rsp, rflags;
        uint64_t rax, rbx, rcx, rdx;
        uint64_t rsi, rdi;
        uint64_t rbp;
    } regs;
    void* manifest; // stub pointer a manifest_t futura
    uint64_t* mapped_pages; // array di indirizzi virtuali pagina mappati (code+data+stack)
    uint32_t mapped_page_count; // numero di pagine
} process_t;

int process_init_system(void); // init table
process_t* process_create_from_elf(const void* elf_buf, size_t size);
void process_print(const process_t* p);
process_t* process_get_last(void);
int process_destroy(process_t* p);

#endif // PROCESS_H
