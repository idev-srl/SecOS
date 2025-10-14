#include "process.h"
#include "elf.h"
#include "heap.h"
#include "terminal.h"
#include "panic.h"
#include "mm/elf_manifest.h"
#include "pmm.h"

#define MAX_PROCESSES 32
static process_t* proc_table[MAX_PROCESSES];
static uint32_t next_pid = 1;
static int proc_inited = 0;

int process_init_system(void) {
    for (int i=0;i<MAX_PROCESSES;i++) proc_table[i]=0;
    proc_inited = 1;
    terminal_writestring("[PROC] init table\n");
    return 0;
}

static int proc_add(process_t* p) {
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (!proc_table[i]) { proc_table[i]=p; return 0; }
    }
    return -1;
}

static void proc_remove(process_t* p) {
    if (!p) return;
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i] == p) { proc_table[i] = NULL; return; }
    }
}

process_t* process_get_last(void) {
    process_t* best = NULL; uint32_t best_pid = 0;
    for (int i=0;i<MAX_PROCESSES;i++) {
        if (proc_table[i] && proc_table[i]->pid > best_pid) { best = proc_table[i]; best_pid = proc_table[i]->pid; }
    }
    return best;
}

process_t* process_create_from_elf(const void* elf_buf, size_t size) {
    if (!proc_inited) process_init_system();
    vmm_space_t* space = vmm_space_create_user();
    if (!space) { terminal_writestring("[PROC] space alloc fail\n"); return NULL; }
    uint64_t entry=0;
    int r = elf_load_image(elf_buf, size, space, &entry);
    if (r != ELF_OK) { terminal_writestring("[PROC] elf load fail\n"); return NULL; }
    uint64_t st_top = vmm_alloc_user_stack_in_space(space, 8);
    process_t* p = (process_t*)kmalloc(sizeof(process_t));
    if (!p) return NULL;
    p->pid = next_pid++;
    p->space = space;
    p->entry = entry;
    p->stack_top = st_top;
    p->kstack_top = 0; // da allocare quando introdurremo scheduler/trap
    p->state = PROC_NEW;
    p->manifest = NULL;
    // Manifest stub
    elf_manifest_t* mf = (elf_manifest_t*)kmalloc(sizeof(elf_manifest_t));
    if (mf && elf_manifest_parse(elf_buf, size, mf) == 0) {
        // Validazione entry e flags
        if (elf_manifest_validate(mf, entry) == MANIFEST_OK) {
            p->manifest = mf;
        } else {
            terminal_writestring("[MANIFEST] validation fail, scarto manifest\n");
            kfree(mf);
        }
    }
    p->regs.rip = entry;
    p->regs.rsp = st_top;
    p->regs.rflags = 0x202; // IF abilitato default
    p->regs.rax = p->regs.rbx = p->regs.rcx = p->regs.rdx = 0;
    p->regs.rsi = p->regs.rdi = p->regs.rbp = 0;
    if (proc_add(p)!=0) { terminal_writestring("[PROC] table full\n"); }
    // Hardening mapping condiviso
    vmm_harden_user_space(space);
    terminal_writestring("[PROC] creato PID=");
    char hx[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->pid>>i)&0xF]);
    terminal_writestring(" entry="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(entry>>i)&0xF]);
    terminal_writestring(" stack_top="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(st_top>>i)&0xF]); terminal_writestring("\n");
    return p;
}

void process_print(const process_t* p) {
    if (!p) return;
    terminal_writestring("[PROC] PID=");
    char hx[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->pid>>i)&0xF]);
    terminal_writestring(" entry="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->entry>>i)&0xF]);
    terminal_writestring(" stack_top="); for(int i=60;i>=0;i-=4) terminal_putchar(hx[(p->stack_top>>i)&0xF]);
    terminal_writestring(" state="); terminal_putchar('0'+p->state);
    terminal_writestring("\n");
}

int process_destroy(process_t* p) {
    if (!p) return -1;
    extern int elf_unload_process(process_t* p);
    elf_unload_process(p);
    if (p->manifest) kfree(p->manifest);
    if (p->space) {
        pmm_free_frame((void*)(p->space->pml4_phys & 0x000FFFFFFFFFF000ULL));
        kfree(p->space);
    }
    proc_remove(p);
    kfree(p);
    terminal_writestring("[PROC] distrutto\n");
    return 0;
}
