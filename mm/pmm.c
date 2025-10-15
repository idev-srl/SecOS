#include "pmm.h"
#include "multiboot.h"
#include "multiboot2.h"
#include "terminal.h"
// print_hex definita in kernel, forward decl per debug
extern void print_hex(uint64_t value);

// Bitmap per tracciare frame liberi/usati
static uint32_t* frame_bitmap = NULL;
static uint64_t total_frames = 0;
static uint64_t used_frames = 0;
static uint64_t total_memory = 0;
static uint64_t max_phys_addr_seen = 0;

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

// Struttura semplice per regioni di memoria disponibili raccolte da loader
struct avail_region { uint64_t addr; uint64_t len; };

static void pmm_build_from_regions(struct avail_region* regions, int region_count) {
    uint64_t max_addr = 0;
    total_memory = 0;
    for (int i=0;i<region_count;i++) {
        uint64_t end = regions[i].addr + regions[i].len;
        if (end > max_addr) max_addr = end;
        total_memory += regions[i].len;
    }
    // Costruzione bitmap frame
    // Limite di frame effettivamente mappati in identity (512MB => 256 * 2MB)
    uint64_t mapped_limit = 512ULL * 1024 * 1024;
    total_frames = (max_addr + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE; // round up
    uint64_t mapped_frames_limit = mapped_limit / PMM_FRAME_SIZE;
    if (total_frames > mapped_frames_limit) total_frames = mapped_frames_limit;
    max_phys_addr_seen = max_addr;
    uint64_t bitmap_size = (total_frames + 7) / 8; // bytes rounded up
    terminal_writestring("[PMM] max_addr="); print_hex(max_addr); terminal_writestring(" total_frames="); {
        char buf[32]; itoa_dec(total_frames, buf); terminal_writestring(buf); }
    terminal_writestring(" bitmap_size="); { char buf2[32]; itoa_dec(bitmap_size, buf2); terminal_writestring(buf2); terminal_writestring(" bytes\n"); }
    frame_bitmap = (uint32_t*)((uint64_t)&_kernel_end);
    // Azzera bitmap
    uint64_t dwords = (bitmap_size + 3) / 4;
    for (uint64_t i = 0; i < dwords; i++) {
        frame_bitmap[i] = 0x00000000;
    }
    used_frames = 0;

    // Libera regioni
    uint64_t free_frames_before = used_frames; // usato per fallback
    for (int r=0;r<region_count;r++) {
        uint64_t start_frame = regions[r].addr / PMM_FRAME_SIZE;
        uint64_t end_frame   = (regions[r].addr + regions[r].len) / PMM_FRAME_SIZE;
        if (end_frame > total_frames) end_frame = total_frames; // clamp to discovered limit
        // Non liberare frame oltre mapping identity corrente
        uint64_t mapped_frames = mapped_limit / PMM_FRAME_SIZE;
        if (end_frame > mapped_frames) end_frame = mapped_frames;
        if (regions[r].len == 0) continue;
    // Log ridotto regioni (solo indice e dimensione in MB)
    terminal_writestring("[PMM] region "); { char b[32]; itoa_dec(r, b); terminal_writestring(b); }
    terminal_writestring(": sizeMB="); { char b1[32]; itoa_dec(regions[r].len / 1024 / 1024, b1); terminal_writestring(b1);} terminal_writestring("\n");
        for (uint64_t f=start_frame; f<end_frame; f++) {
            // Mark free (already zero) just ensure we don't double count
            if (bitmap_test(f)) {
                bitmap_clear(f);
                used_frames--;
            }
        }
    }
    // Proteggi area kernel+bitmap
    uint64_t kernel_end_frame = ((uint64_t)frame_bitmap + bitmap_size) / PMM_FRAME_SIZE + 1;
    for (uint64_t f=0; f<kernel_end_frame; f++) {
        if (!bitmap_test(f)) { bitmap_set(f); used_frames++; }
    }
    // Fallback: se nessun frame libero (tutti occupati) crea regione sintetica dopo kernel entro mapped limit
    if (used_frames == 0 || used_frames == total_frames || pmm_get_free_memory() == 0) {
        terminal_writestring("[PMM][WARN] Nessun frame libero dalle regioni; applico fallback sintetico\n");
        uint64_t fallback_start = ((uint64_t)frame_bitmap + bitmap_size + PMM_FRAME_SIZE -1) & ~(PMM_FRAME_SIZE-1);
        uint64_t fallback_end = mapped_limit;
        uint64_t start_f = fallback_start / PMM_FRAME_SIZE;
        uint64_t end_f = fallback_end / PMM_FRAME_SIZE;
        if (end_f > total_frames) end_f = total_frames;
        for (uint64_t f = start_f; f < end_f; f++) {
            if (!bitmap_test(f)) {
                // leave as free
            }
        }
        // Reconta used frames (proteggendo kernel) - semplice: marca kernel frames e lascia resto libero
        used_frames = 0;
        for (uint64_t f=0; f<kernel_end_frame; f++) { if (!bitmap_test(f)) { bitmap_set(f); used_frames++; } }
    }
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[OK] PMM inizializzato\n");
    pmm_print_stats();
}

uint64_t pmm_get_max_phys(void) { return max_phys_addr_seen; }

// Inizializza il PMM (Multiboot1)
void pmm_init(void* mboot_info_ptr) {
    if (!mboot_info_ptr) { terminal_writestring("[ERROR] Multiboot info NULL!\n"); return; }
    struct multiboot_info* mboot = (struct multiboot_info*)mboot_info_ptr;
    terminal_writestring("[MB1] flags=0x"); {
        char hex[9];
        uint32_t f = mboot->flags; for (int i=7;i>=0;i--){ uint8_t ny=(f & 0xF); hex[i] = "0123456789ABCDEF"[ny]; f >>=4; } hex[8]='\0'; terminal_writestring(hex); terminal_writestring("\n"); }
    if (!(mboot->flags & (1 << 6))) {
        terminal_writestring("[WARN] Nessuna memory map MB1, fallback sintetico\n");
        struct avail_region regions[1];
        // Usa mem_upper se disponibile (flag bit 0 indica mem_lower/upper validi)
        uint64_t upper_kb = 0;
        if (mboot->flags & 1) upper_kb = mboot->mem_upper;
        if (upper_kb == 0) upper_kb = 64 * 1024; // fallback 64MB
        regions[0].addr = 0x00100000; // 1MB
        regions[0].len = (upper_kb * 1024) - 0x00100000;
        pmm_build_from_regions(regions, 1);
        return;
    }
    struct multiboot_mmap_entry* mmap = (struct multiboot_mmap_entry*)((uint64_t)mboot->mmap_addr);
    struct multiboot_mmap_entry* mmap_end = (struct multiboot_mmap_entry*)((uint64_t)mboot->mmap_addr + mboot->mmap_length);
    struct avail_region regions[64]; int rc=0;
    while (mmap < mmap_end && rc < 64) {
        if (mmap->type == MULTIBOOT_MEMORY_AVAILABLE) {
            regions[rc].addr = mmap->addr;
            regions[rc].len  = mmap->len;
            rc++;
        }
        mmap = (struct multiboot_mmap_entry*)((uint64_t)mmap + mmap->size + sizeof(mmap->size));
    }
    // Clamp region length to 128MB identity map for early phase
    uint64_t identity_limit = 128ULL * 1024 * 1024;
    for (int r=0; r<rc; r++) {
        uint64_t end = regions[r].addr + regions[r].len;
        if (end > identity_limit) {
            if (regions[r].addr >= identity_limit) { regions[r].len = 0; }
            else regions[r].len = identity_limit - regions[r].addr;
        }
    }
    pmm_build_from_regions(regions, rc);
}

// Inizializza il PMM (Multiboot2)
void pmm_init_mb2(void* mb2_info_ptr) {
    if (!mb2_info_ptr) { terminal_writestring("[ERROR] MB2 info NULL!\n"); return; }
    uint8_t* base = (uint8_t*)mb2_info_ptr;
    uint32_t total_size = *(uint32_t*)base;
    (void)total_size;
    uint8_t* ptr = base + 8; // skip size + reserved
    struct avail_region regions[128]; int rc=0;
    while (1) {
        struct multiboot2_tag* tag = (struct multiboot2_tag*)ptr;
        if (tag->type == 0) break; // end
        if (tag->type == 6) { // mmap
            struct multiboot2_tag_mmap* mtag = (struct multiboot2_tag_mmap*)tag;
            uint8_t* entry_ptr = (uint8_t*)mtag + sizeof(struct multiboot2_tag_mmap);
            uint8_t* entry_end = (uint8_t*)mtag + mtag->size;
            while (entry_ptr + mtag->entry_size <= entry_end && rc < 128) {
                struct multiboot2_mmap_entry* e = (struct multiboot2_mmap_entry*)entry_ptr;
                if (e->type == 1) { // available
                    regions[rc].addr = e->addr;
                    regions[rc].len  = e->len;
                    rc++;
                }
                entry_ptr += mtag->entry_size;
            }
        }
        // advance aligned
        ptr += (tag->size + 7) & ~7;
    }
    if (rc == 0) {
        terminal_writestring("[WARN] Nessuna regione MB2 riportata, fallback sintetico 1MB..64MB\n");
        regions[0].addr = 0x00100000ULL;
        regions[0].len  = 63ULL * 1024 * 1024; // 63MB sopra 1MB
        rc = 1;
    } else if (rc == 1 && regions[0].addr == 0 && regions[0].len < 0x100000) {
        // Solo low memory, aggiungi regione alta sintetica
        terminal_writestring("[WARN] Solo low memory in mmap; aggiungo regione sintetica 1MB..256MB\n");
        regions[1].addr = 0x00100000ULL;
        regions[1].len  = 255ULL * 1024 * 1024; // fino a 256MB totale
        rc = 2;
    }
    pmm_build_from_regions(regions, rc);
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