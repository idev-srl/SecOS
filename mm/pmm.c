#include "pmm.h"
#include "multiboot.h"
#include "terminal.h"

// Bitmap per tracciare frame liberi/usati
static uint32_t* frame_bitmap = NULL;
static uint64_t total_frames = 0;
static uint64_t used_frames = 0;
static uint64_t total_memory = 0;

// Posizione del kernel (definita nel linker)
extern uint32_t _kernel_end;

// Helper: imposta un bit nel bitmap
static inline void bitmap_set(uint32_t frame) {
    uint32_t idx = frame / 32;
    uint32_t bit = frame % 32;
    frame_bitmap[idx] |= (1 << bit);
}

// Helper: pulisce un bit nel bitmap
static inline void bitmap_clear(uint32_t frame) {
    uint32_t idx = frame / 32;
    uint32_t bit = frame % 32;
    frame_bitmap[idx] &= ~(1 << bit);
}

// Helper: testa un bit nel bitmap
static inline bool bitmap_test(uint32_t frame) {
    uint32_t idx = frame / 32;
    uint32_t bit = frame % 32;
    return (frame_bitmap[idx] & (1 << bit)) != 0;
}

// Trova il primo frame libero
static uint32_t find_free_frame(void) {
    for (uint32_t i = 0; i < total_frames; i++) {
        if (!bitmap_test(i)) {
            return i;
        }
    }
    return (uint32_t)-1;
}

// Converti numero in stringa
static void itoa_dec(uint64_t value, char* buffer) {
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    
    char temp[32];
    int i = 0;
    
    while (value > 0) {
        temp[i++] = '0' + (value % 10);
        value /= 10;
    }
    
    int j = 0;
    while (i > 0) {
        buffer[j++] = temp[--i];
    }
    buffer[j] = '\0';
}

// Inizializza il PMM
void pmm_init(void* mboot_info_ptr) {
    if (mboot_info_ptr == NULL) {
        terminal_writestring("[ERROR] Multiboot info NULL!\n");
        return;
    }
    
    struct multiboot_info* mboot = (struct multiboot_info*)mboot_info_ptr;
    
    // Verifica che abbiamo la memory map
    if (!(mboot->flags & (1 << 6))) {
        terminal_writestring("[ERROR] Nessuna memory map disponibile!\n");
        return;
    }
    
    // Trova la memoria totale disponibile
    struct multiboot_mmap_entry* mmap = (struct multiboot_mmap_entry*)((uint64_t)mboot->mmap_addr);
    struct multiboot_mmap_entry* mmap_end = (struct multiboot_mmap_entry*)((uint64_t)mboot->mmap_addr + mboot->mmap_length);
    
    uint64_t max_addr = 0;
    
    while (mmap < mmap_end) {
        if (mmap->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uint64_t end_addr = mmap->addr + mmap->len;
            if (end_addr > max_addr) {
                max_addr = end_addr;
            }
            total_memory += mmap->len;
        }
        mmap = (struct multiboot_mmap_entry*)((uint64_t)mmap + mmap->size + sizeof(mmap->size));
    }
    
    // Calcola numero di frame
    total_frames = max_addr / PMM_FRAME_SIZE;
    
    // Alloca il bitmap subito dopo il kernel
    uint64_t bitmap_size = (total_frames / 8) + 1;  // 1 bit per frame
    frame_bitmap = (uint32_t*)((uint64_t)&_kernel_end);
    
    // Azzera il bitmap (tutti i frame marcati come usati)
    for (uint64_t i = 0; i < bitmap_size / 4; i++) {
        frame_bitmap[i] = 0xFFFFFFFF;
    }
    
    used_frames = total_frames;
    
    // Marca come liberi solo i frame nelle regioni disponibili
    mmap = (struct multiboot_mmap_entry*)((uint64_t)mboot->mmap_addr);
    
    while (mmap < mmap_end) {
        if (mmap->type == MULTIBOOT_MEMORY_AVAILABLE) {
            uint64_t start_frame = mmap->addr / PMM_FRAME_SIZE;
            uint64_t end_frame = (mmap->addr + mmap->len) / PMM_FRAME_SIZE;
            
            for (uint64_t frame = start_frame; frame < end_frame; frame++) {
                bitmap_clear(frame);
                used_frames--;
            }
        }
        mmap = (struct multiboot_mmap_entry*)((uint64_t)mmap + mmap->size + sizeof(mmap->size));
    }
    
    // Marca come usati i primi MB (kernel + bitmap)
    uint64_t kernel_end_frame = ((uint64_t)frame_bitmap + bitmap_size) / PMM_FRAME_SIZE + 1;
    for (uint64_t frame = 0; frame < kernel_end_frame; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            used_frames++;
        }
    }
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[OK] PMM inizializzato\n");
    pmm_print_stats();
}

// Alloca un frame
void* pmm_alloc_frame(void) {
    uint32_t frame = find_free_frame();
    
    if (frame == (uint32_t)-1) {
        return NULL;  // Out of memory
    }
    
    bitmap_set(frame);
    used_frames++;
    
    return (void*)((uint64_t)frame * PMM_FRAME_SIZE);
}

// Libera un frame
void pmm_free_frame(void* addr) {
    uint32_t frame = (uint64_t)addr / PMM_FRAME_SIZE;
    
    if (!bitmap_test(frame)) {
        return;  // Già libero
    }
    
    bitmap_clear(frame);
    used_frames--;
}

// Ottieni memoria totale
uint64_t pmm_get_total_memory(void) {
    return total_memory;
}

// Ottieni memoria usata
uint64_t pmm_get_used_memory(void) {
    return used_frames * PMM_FRAME_SIZE;
}

// Ottieni memoria libera
uint64_t pmm_get_free_memory(void) {
    return (total_frames - used_frames) * PMM_FRAME_SIZE;
}

// Stampa statistiche
void pmm_print_stats(void) {
    char buffer[32];
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("     Memoria totale:   ");
    itoa_dec(total_memory / 1024 / 1024, buffer);
    terminal_writestring(buffer);
    terminal_writestring(" MB\n");
    
    terminal_writestring("     Memoria usata:    ");
    itoa_dec(pmm_get_used_memory() / 1024 / 1024, buffer);
    terminal_writestring(buffer);
    terminal_writestring(" MB\n");
    
    terminal_writestring("     Memoria libera:   ");
    itoa_dec(pmm_get_free_memory() / 1024 / 1024, buffer);
    terminal_writestring(buffer);
    terminal_writestring(" MB\n");
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}