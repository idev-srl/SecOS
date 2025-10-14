#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Dimensione frame (4KB)
#define PMM_FRAME_SIZE 4096

// Inizializza il PMM con la memory map
void pmm_init(void* mboot_info);

// Alloca un frame fisico
void* pmm_alloc_frame(void);

// Libera un frame fisico
void pmm_free_frame(void* addr);

// Ottieni informazioni memoria
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_used_memory(void);
uint64_t pmm_get_free_memory(void);

// Debug
void pmm_print_stats(void);

#endif