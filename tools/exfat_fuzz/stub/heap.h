/* Host stub of mm/heap.h for the exFAT fuzz harness. */
#pragma once
#include <stdlib.h>
static inline void* kmalloc(size_t n){ return malloc(n); }
static inline void kfree(void* p){ free(p); }
