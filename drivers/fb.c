#include "fb.h"
#include "terminal.h"
#include "multiboot2.h"
#include "vmm.h"
#include "pmm.h"

#if ENABLE_FB

static framebuffer_info_t g_fb;
static int g_fb_ready = 0;
static int g_fb_phys_only = 0; // set if high address not yet virtually mapped
// Forward declare phys_to_virt
extern uint64_t phys_to_virt(uint64_t phys);

void fb_finalize_mapping(void) {
    if (!g_fb_ready) return;
    if (!g_fb_phys_only) return;
    // Assicura che physmap copra l'intero framebuffer
    uint64_t fb_end = g_fb.addr + (uint64_t)g_fb.pitch * g_fb.height;
    vmm_extend_physmap(fb_end);
    // Assume physmap ora copre il range richiesto
    g_fb.virt_addr = phys_to_virt(g_fb.addr);
    g_fb_phys_only = 0;
}

int fb_init(uint32_t mb2_info) {
    uint8_t* base = (uint8_t*)(uint64_t)mb2_info;
    uint32_t total_size = *(uint32_t*)base;
    uint8_t* tags = base + 8;
    uint8_t* end = base + total_size;
    struct multiboot2_tag_framebuffer* found = NULL;
    __asm__ volatile("mov $'i', %al; out %al, $0xE9");
    while (tags < end) {
        struct multiboot2_tag* tag = (struct multiboot2_tag*)tags;
        if (tag->type == MULTIBOOT2_TAG_END) break;
        if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) { __asm__ volatile("mov $'F', %al; out %al, $0xE9"); }
        if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
            found = (struct multiboot2_tag_framebuffer*)tag;
            break;
        }
        tags += (tag->size + 7) & ~7;
    }
    if (!found) {
        __asm__ volatile("mov $'0', %al; out %al, $0xE9");
        terminal_writestring("[FB] Tag framebuffer MB2 non trovato\n");
        return -1;
    }
    __asm__ volatile("mov $'1', %al; out %al, $0xE9");
    g_fb.addr  = found->addr;
    g_fb.virt_addr = 0; // will set later if needed
    g_fb.pitch = found->pitch;
    g_fb.width = found->width;
    g_fb.height= found->height;
    g_fb.bpp   = found->bpp;
    g_fb.type  = found->type_fb;
    // Emit quick hex nybbles of low 8 bytes of addr via port (addr & 0xFF then pitch &0xFF etc.)
    uint64_t a = g_fb.addr;
    for(int k=0;k<2;k++){ uint8_t n = (a >> (k*4)) & 0xF; __asm__ volatile("out %0, $0xE9"::"a"((uint8_t)("0123456789ABCDEF"[n]))); }
    __asm__ volatile("mov $'A', %al; out %al, $0xE9");
    uint32_t pw = g_fb.pitch; for(int k=0;k<2;k++){ uint8_t n=(pw>>(k*4))&0xF; __asm__ volatile("out %0,$0xE9"::"a"((uint8_t)("0123456789ABCDEF"[n]))); }
    __asm__ volatile("mov $'P', %al; out %al, $0xE9");
    // Mask sizes/positions se disponibili (solo se type_fb==1 RGB, in MB2 sono dopo struttura base)
    if (found->type_fb == 1) {
        // Calcolo offset dei mask: subito dopo campi base
        uint8_t* extra = (uint8_t*)found + sizeof(struct multiboot2_tag_framebuffer);
        g_fb.red_mask_size = extra[0];
        g_fb.red_mask_pos  = extra[1];
        g_fb.green_mask_size = extra[2];
        g_fb.green_mask_pos  = extra[3];
        g_fb.blue_mask_size = extra[4];
        g_fb.blue_mask_pos  = extra[5];
    } else {
        g_fb.red_mask_size = g_fb.red_mask_pos = 0;
        g_fb.green_mask_size = g_fb.green_mask_pos = 0;
        g_fb.blue_mask_size = g_fb.blue_mask_pos = 0;
    }

    terminal_writestring("[FB] Framebuffer: ");
    terminal_writestring("" );
    terminal_writestring("addr=");
    print_hex(g_fb.addr);
    terminal_writestring(" pitch=");
    char buf[16];
    // simple decimal print
    int v=g_fb.pitch; int i=0; char tmp[16]; if(v==0){ buf[0]='0'; buf[1]='\0'; } else { while(v>0){ tmp[i++]='0'+(v%10); v/=10; } for(int j=0;j<i;j++){ buf[j]=tmp[i-1-j]; } buf[i]='\0'; }
    terminal_writestring(buf);
    terminal_writestring(" \n");

    // If address above 16MB identity region, delay usage until physmap
    if (g_fb.addr >= (16ULL*1024*1024)) {
        g_fb_phys_only = 1;
        terminal_writestring("[FB] Alto indirizzo, rinvio accessi finché physmap non pronta\n");
    }
    g_fb_ready = 1;
    __asm__ volatile("mov $'2', %al; out %al, $0xE9");
    return 0;
}

int fb_get_info(framebuffer_info_t* out) {
    if (!g_fb_ready || !out) return 0;
    *out = g_fb;
    return 1;
}

static inline uint32_t pack_rgb(uint8_t r,uint8_t g,uint8_t b){ return ((uint32_t)r<<16)|((uint32_t)g<<8)|((uint32_t)b); }

void fb_clear(uint32_t color) {
    if(!g_fb_ready) return;
    if (g_fb_phys_only) return; // avoid fault
    uint8_t* base = (uint8_t*)(g_fb.virt_addr ? g_fb.virt_addr : g_fb.addr);
    for (uint32_t y=0; y<g_fb.height; y++) {
        uint32_t* row = (uint32_t*)(base + y * g_fb.pitch);
        for (uint32_t x=0; x<g_fb.width; x++) {
            row[x] = color;
        }
    }
}

void fb_putpixel(int x, int y, uint32_t color) {
    if(!g_fb_ready) return;
    if(x<0||y<0||x>=(int)g_fb.width||y>=(int)g_fb.height) return;
    if (g_fb_phys_only) return;
    uint8_t* base = (uint8_t*)(g_fb.virt_addr ? g_fb.virt_addr : g_fb.addr);
    uint32_t* row = (uint32_t*)(base + y * g_fb.pitch);
    row[x] = color;
}

void fb_draw_test_pattern(void) {
    if(!g_fb_ready || g_fb_phys_only) return;
    uint8_t* base = (uint8_t*)(g_fb.virt_addr ? g_fb.virt_addr : g_fb.addr);
    uint8_t bpp = g_fb.bpp;
    for(uint32_t y=0; y<g_fb.height; y++) {
        uint8_t* row = base + y * g_fb.pitch;
        for(uint32_t x=0; x<g_fb.width; x++) {
            uint8_t r = (uint8_t)((x * 255) / g_fb.width);
            uint8_t g = (uint8_t)((y * 255) / g_fb.height);
            uint8_t b = (uint8_t)(((x+y) * 255) / (g_fb.width + g_fb.height));
            if (bpp == 32) {
                uint32_t* px = (uint32_t*)(row + x*4); *px = (r<<16)|(g<<8)|b;
            } else if (bpp == 24) {
                uint8_t* px = row + x*3; px[0]=b; px[1]=g; px[2]=r;
            } else if (bpp == 16) {
                uint16_t* px = (uint16_t*)(row + x*2);
                uint16_t val = ((r>>3)<<11)|((g>>2)<<5)|(b>>3);
                *px = val;
            }
        }
    }
}

void fb_debug_fill(uint32_t color_rgb) {
    if(!g_fb_ready || g_fb_phys_only) return;
    uint8_t* base = (uint8_t*)(g_fb.virt_addr ? g_fb.virt_addr : g_fb.addr);
    uint8_t bpp = g_fb.bpp;
    for (uint32_t y=0; y<g_fb.height; y++) {
        uint8_t* row = base + y * g_fb.pitch;
        if (bpp == 32) {
            uint32_t* dst = (uint32_t*)row; for(uint32_t x=0;x<g_fb.width;x++) dst[x]=color_rgb;
        } else if (bpp == 24) {
            uint8_t b = color_rgb & 0xFF; uint8_t g = (color_rgb>>8)&0xFF; uint8_t r=(color_rgb>>16)&0xFF;
            for(uint32_t x=0;x<g_fb.width;x++){ uint8_t* px=row + x*3; px[0]=b; px[1]=g; px[2]=r; }
        } else if (bpp == 16) {
            uint8_t r=(color_rgb>>16)&0xFF, g=(color_rgb>>8)&0xFF, b=color_rgb &0xFF;
            uint16_t val = ((r>>3)<<11)|((g>>2)<<5)|(b>>3);
            uint16_t* dst = (uint16_t*)row; for(uint32_t x=0;x<g_fb.width;x++) dst[x]=val;
        }
    }
}

#endif // ENABLE_FB
