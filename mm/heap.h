#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

// Inizializza l'heap
void heap_init(void);

// Alloca memoria
void* kmalloc(size_t size);

// Alloca memoria allineata
void* kmalloc_aligned(size_t size, size_t alignment);

// Libera memoria
void kfree(void* ptr);

// Statistiche heap
void heap_print_stats(void);

#endif