#include "elf.h"
#include "vmm.h"
#include "terminal.h"
#include "process.h"
#include "pmm.h"
#include "heap.h"

#define ADDRESS_MASK 0x000FFFFFFFFFF000ULL

// Semplice elfunload: smappa pagine user code/data del processo (non gestisce refcount né stack condivisi)
// Assunzioni: process_t->space esiste, segmenti ELF sono contigui a partire da USER_CODE_BASE / USER_DATA_BASE.
// Reale implementazione dovrebbe tracciare ogni pagina mappata. Qui usiamo heuristica su range.

#define UNLOAD_CODE_RANGE   (0x1000 * 64)   // 64 pagine max codice
#define UNLOAD_DATA_RANGE   (0x1000 * 128)  // 128 pagine max dati
#define UNLOAD_STACK_RANGE  (0x1000 * 32)   // 32 pagine max stack (incluse extra non mappate)

static int unmap_range_space(vmm_space_t* space, uint64_t start, uint64_t length, int* pages) {
    for (uint64_t va = start; va < start + length; va += 0x1000ULL) {
        if (vmm_unmap_in_space(space, va) == 0) {
            (*pages)++;
        }
    }
    return 0;
}

int elf_unload_process(process_t* p) {
    if (!p) return -1;
    vmm_space_t* space = p->space;
    if (!space) return -2;
    terminal_writestring("[ELFUNLOAD] start PID=");
    char hx[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->pid>>i)&0xF]); terminal_writestring("\n");
    int pages_freed = 0;
    unmap_range_space(space, USER_CODE_BASE, UNLOAD_CODE_RANGE, &pages_freed);
    unmap_range_space(space, USER_DATA_BASE, UNLOAD_DATA_RANGE, &pages_freed);
    unmap_range_space(space, USER_STACK_TOP - UNLOAD_STACK_RANGE, UNLOAD_STACK_RANGE, &pages_freed);
    terminal_writestring("[ELFUNLOAD] done pages=");
    char hx2[]="0123456789ABCDEF"; int v=pages_freed; if (v==0) terminal_putchar('0'); else {
        char tmp[16]; int idx=0; while(v>0){ tmp[idx++]=hx2[v & 0xF]; v >>=4; }
        while(idx>0) terminal_putchar(tmp[--idx]);
    }
    terminal_writestring("\n");
    return 0;
}
