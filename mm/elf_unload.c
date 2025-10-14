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

// Ora usiamo tracking reale: PCB contiene p->mapped_pages e p->mapped_page_count

int elf_unload_process(process_t* p) {
    if (!p) return -1;
    vmm_space_t* space = p->space;
    if (!space) return -2;
    terminal_writestring("[ELFUNLOAD] start PID=");
    char hx[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) terminal_putchar(hx[(p->pid>>i)&0xF]); terminal_writestring("\n");
    int pages_freed = 0;
    if (p->mapped_pages) {
        for (uint32_t i=0;i<p->mapped_page_count;i++) {
            uint64_t va = p->mapped_pages[i];
            if (vmm_unmap_in_space(space, va) == 0) pages_freed++;
        }
    }
    terminal_writestring("[ELFUNLOAD] done pages=");
    char hx2[]="0123456789ABCDEF"; int v=pages_freed; if (v==0) terminal_putchar('0'); else {
        char tmp[16]; int idx=0; while(v>0){ tmp[idx++]=hx2[v & 0xF]; v >>=4; }
        while(idx>0) terminal_putchar(tmp[--idx]);
    }
    terminal_writestring("\n");
    // Libera lista pagine
    if (p->mapped_pages) { kfree(p->mapped_pages); p->mapped_pages=NULL; p->mapped_page_count=0; }
    return 0;
}
