#include "heap.h"
#include "pmm.h"
#include "terminal.h"

// Header per ogni blocco
typedef struct heap_block {
    size_t size;
    bool is_free;
    struct heap_block* next;
} heap_block_t;

#define HEAP_BLOCK_HEADER_SIZE sizeof(heap_block_t)

static heap_block_t* heap_start = NULL;
static uint64_t total_allocated = 0;
static uint64_t total_freed = 0;

// Helper per allineare indirizzi
static inline size_t align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
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

// Inizializza l'heap
void heap_init(void) {
    // Alloca il primo frame per l'heap
    heap_start = (heap_block_t*)pmm_alloc_frame();
    
    if (heap_start == NULL) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[ERROR] Impossibile allocare heap iniziale!\n");
        terminal_writestring("[DEBUG] pmm_alloc_frame() ha ritornato NULL\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    heap_start->size = PMM_FRAME_SIZE - HEAP_BLOCK_HEADER_SIZE;
    heap_start->is_free = true;
    heap_start->next = NULL;
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[OK] Heap inizializzato @ ");
    
    // Stampa l'indirizzo dell'heap (debug)
    char hex_chars[] = "0123456789ABCDEF";
    uint64_t addr = (uint64_t)heap_start;
    for (int i = 60; i >= 0; i -= 4) {
        char c = hex_chars[(addr >> i) & 0xF];
        terminal_putchar(c);
    }
    terminal_writestring("\n");
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

// Espandi l'heap allocando un nuovo frame
static heap_block_t* expand_heap(size_t required_size) {
    // Trova l'ultimo blocco
    heap_block_t* current = heap_start;
    while (current->next != NULL) {
        current = current->next;
    }
    
    // Alloca un nuovo frame
    void* new_frame = pmm_alloc_frame();
    if (new_frame == NULL) {
        return NULL;
    }
    
    // Crea un nuovo blocco nel frame allocato
    heap_block_t* new_block = (heap_block_t*)new_frame;
    new_block->size = PMM_FRAME_SIZE - HEAP_BLOCK_HEADER_SIZE;
    new_block->is_free = true;
    new_block->next = NULL;
    
    current->next = new_block;
    
    return new_block;
}

// Alloca memoria
void* kmalloc(size_t size) {
    // DEBUG
    terminal_writestring("[kmalloc] Entry, size=");
    char buf[16];
    itoa_dec(size, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    if (size == 0) {
        terminal_writestring("[kmalloc] Size 0, return NULL\n");
        return NULL;
    }
    
    // Verifica che l'heap sia inizializzato
    if (heap_start == NULL) {
        terminal_writestring("[kmalloc] heap_start is NULL!\n");
        return NULL;
    }
    
    terminal_writestring("[kmalloc] heap_start OK\n");
    
    // Allinea la dimensione a 8 byte
    size = align_up(size, 8);
    
    terminal_writestring("[kmalloc] Searching for free block...\n");
    
    heap_block_t* current = heap_start;
    
    // Cerca un blocco libero abbastanza grande
    while (current != NULL) {
        terminal_writestring("[kmalloc] Checking block\n");
        
        if (current->is_free && current->size >= size) {
            terminal_writestring("[kmalloc] Found free block!\n");
            
            // Trovato un blocco adatto
            current->is_free = false;
            total_allocated += current->size;
            
            terminal_writestring("[kmalloc] Returning pointer\n");
            
            // Ritorna il puntatore ai dati (dopo l'header)
            return (void*)((uint8_t*)current + HEAP_BLOCK_HEADER_SIZE);
        }
        
        current = current->next;
    }
    
    terminal_writestring("[kmalloc] No free block, need to expand\n");
    
    // Nessun blocco disponibile
    return NULL;
}

// Alloca memoria allineata
void* kmalloc_aligned(size_t size, size_t alignment) {
    // Alloca extra spazio per l'allineamento
    void* ptr = kmalloc(size + alignment);
    if (ptr == NULL) {
        return NULL;
    }
    
    // Allinea il puntatore
    uint64_t aligned = align_up((uint64_t)ptr, alignment);
    return (void*)aligned;
}

// Libera memoria
void kfree(void* ptr) {
    if (ptr == NULL) {
        return;
    }
    
    // Ottieni l'header del blocco
    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - HEAP_BLOCK_HEADER_SIZE);
    
    if (block->is_free) {
        return;  // Già libero
    }
    
    block->is_free = true;
    total_freed += block->size;
    
    // Unisci blocchi liberi adiacenti (coalescing)
    heap_block_t* current = heap_start;
    
    while (current != NULL && current->next != NULL) {
        if (current->is_free && current->next->is_free) {
            // Unisci i due blocchi
            current->size += HEAP_BLOCK_HEADER_SIZE + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}

// Stampa statistiche heap
void heap_print_stats(void) {
    char buffer[32];
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\n=== Statistiche Heap ===\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    terminal_writestring("Allocata:   ");
    itoa_dec(total_allocated, buffer);
    terminal_writestring(buffer);
    terminal_writestring(" bytes\n");
    
    terminal_writestring("Liberata:   ");
    itoa_dec(total_freed, buffer);
    terminal_writestring(buffer);
    terminal_writestring(" bytes\n");
    
    terminal_writestring("In uso:     ");
    itoa_dec(total_allocated - total_freed, buffer);
    terminal_writestring(buffer);
    terminal_writestring(" bytes\n\n");
}