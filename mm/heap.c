/*

 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "heap.h"
#include "pmm.h"
#include "terminal.h"

// Header for each heap block
typedef struct heap_block {
    size_t size;
    bool is_free;
    struct heap_block* next;
} heap_block_t;

#define HEAP_BLOCK_HEADER_SIZE sizeof(heap_block_t)

static heap_block_t* heap_start = NULL;
static uint64_t total_allocated = 0;
static uint64_t total_freed = 0;

// Helper to align addresses
static inline size_t align_up(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

// Convert number to decimal string
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

// Initialize heap
void heap_init(void) {
    // Allocate the first frame for the heap
    heap_start = (heap_block_t*)pmm_alloc_frame();
    
    if (heap_start == NULL) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[ERROR] Unable to allocate initial heap!\n");
        terminal_writestring("[DEBUG] pmm_alloc_frame() returned NULL\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return;
    }
    
    heap_start->size = PMM_FRAME_SIZE - HEAP_BLOCK_HEADER_SIZE;
    heap_start->is_free = true;
    heap_start->next = NULL;
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[OK] Heap initialized @ ");
    
    // Print heap address (debug)
    char hex_chars[] = "0123456789ABCDEF";
    uint64_t addr = (uint64_t)heap_start;
    for (int i = 60; i >= 0; i -= 4) {
        char c = hex_chars[(addr >> i) & 0xF];
        terminal_putchar(c);
    }
    terminal_writestring("\n");
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

// Expand heap by allocating a new physical frame
static heap_block_t* expand_heap(size_t required_size) {
    // Find last block
    heap_block_t* current = heap_start;
    while (current->next != NULL) {
        current = current->next;
    }
    
    // Allocate a new frame
    void* new_frame = pmm_alloc_frame();
    if (new_frame == NULL) {
        return NULL;
    }
    
    // Create a new block in the allocated frame
    heap_block_t* new_block = (heap_block_t*)new_frame;
    new_block->size = PMM_FRAME_SIZE - HEAP_BLOCK_HEADER_SIZE;
    new_block->is_free = true;
    new_block->next = NULL;
    
    current->next = new_block;
    
    return new_block;
}

// Allocate memory
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
    
    // Verify heap is initialized
    if (heap_start == NULL) {
        terminal_writestring("[kmalloc] heap_start is NULL!\n");
        return NULL;
    }
    
    terminal_writestring("[kmalloc] heap_start OK\n");
    
    // Align size to 8 bytes
    size = align_up(size, 8);
    
    terminal_writestring("[kmalloc] Searching for free block...\n");
    
    heap_block_t* current = heap_start;
    
    // Search free block large enough (with potential splitting)
    while (current != NULL) {
        terminal_writestring("[kmalloc] Checking block\n");
        if (current->is_free && current->size >= size) {
            terminal_writestring("[kmalloc] Found free block!\n");
            // If block is much larger, split it
            size_t remaining = current->size - size;
            if (remaining > HEAP_BLOCK_HEADER_SIZE + 16) {
                // Perform split
                uint8_t* new_addr = (uint8_t*)current + HEAP_BLOCK_HEADER_SIZE + size;
                heap_block_t* new_block = (heap_block_t*)new_addr;
                new_block->size = remaining - HEAP_BLOCK_HEADER_SIZE;
                new_block->is_free = true;
                new_block->next = current->next;
                current->next = new_block;
                current->size = size; // resize allocated block
                terminal_writestring("[kmalloc] Block split\n");
            }
            current->is_free = false;
            total_allocated += current->size;
            terminal_writestring("[kmalloc] Returning pointer\n");
            return (void*)((uint8_t*)current + HEAP_BLOCK_HEADER_SIZE);
        }
        current = current->next;
    }
    terminal_writestring("[kmalloc] No free block, need to expand\n");
    // Try to expand the heap
    heap_block_t* new_block = expand_heap(size);
    if (!new_block) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[FAIL] Allocation failed (expand_heap NULL)\n");
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        return NULL;
    }
    // Allocate from the new block (potential split if too large)
    if (new_block->size > size + HEAP_BLOCK_HEADER_SIZE + 16) {
        uint8_t* split_addr = (uint8_t*)new_block + HEAP_BLOCK_HEADER_SIZE + size;
        heap_block_t* tail = (heap_block_t*)split_addr;
        tail->size = new_block->size - size - HEAP_BLOCK_HEADER_SIZE;
        tail->is_free = true;
        tail->next = new_block->next;
        new_block->next = tail;
        new_block->size = size;
        terminal_writestring("[kmalloc] Expanded and split new block\n");
    }
    new_block->is_free = false;
    total_allocated += new_block->size;
    return (void*)((uint8_t*)new_block + HEAP_BLOCK_HEADER_SIZE);
}

// Allocate memory with alignment guarantee
void* kmalloc_aligned(size_t size, size_t alignment) {
    // Allocate extra space for alignment
    void* ptr = kmalloc(size + alignment);
    if (ptr == NULL) {
        return NULL;
    }
    
    // Align pointer
    uint64_t aligned = align_up((uint64_t)ptr, alignment);
    return (void*)aligned;
}

// Free memory
void kfree(void* ptr) {
    if (ptr == NULL) {
        return;
    }
    
    // Get block header
    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - HEAP_BLOCK_HEADER_SIZE);
    
    if (block->is_free) {
        return;  // Already free
    }
    
    block->is_free = true;
    total_freed += block->size;
    
    // Coalesce adjacent free blocks
    heap_block_t* current = heap_start;
    
    while (current != NULL && current->next != NULL) {
        if (current->is_free && current->next->is_free) {
            // Merge blocks
            current->size += HEAP_BLOCK_HEADER_SIZE + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}

// Print heap allocator statistics
void heap_print_stats(void) {
    char buffer[32];
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("\n=== Heap Statistics ===\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    
    terminal_writestring("Allocated:  ");
    itoa_dec(total_allocated, buffer);
    terminal_writestring(buffer);
    terminal_writestring(" bytes\n");
    
    terminal_writestring("Freed:      ");
    itoa_dec(total_freed, buffer);
    terminal_writestring(buffer);
    terminal_writestring(" bytes\n");
    
    terminal_writestring("In use:     ");
    itoa_dec(total_allocated - total_freed, buffer);
    terminal_writestring(buffer);
    terminal_writestring(" bytes\n\n");
}