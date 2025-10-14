#include "fb.h"
#include "terminal.h"
#include "multiboot2.h"
#include "vmm.h"
#include "pmm.h"

#if ENABLE_FB

static framebuffer_info_t g_fb;
static int g_fb_ready = 0;

int fb_init(uint32_t mb2_info) {
    uint8_t* base = (uint8_t*)(uint64_t)mb2_info;
    uint32_t total_size = *(uint32_t*)base;
    uint8_t* tags = base + 8;
    uint8_t* end = base + total_size;
    struct multiboot2_tag_framebuffer* found = NULL;
    while (tags < end) {
        struct multiboot2_tag* tag = (struct multiboot2_tag*)tags;
        if (tag->type == MULTIBOOT2_TAG_END) break;
        if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
            found = (struct multiboot2_tag_framebuffer*)tag;
            break;
        }
        tags += (tag->size + 7) & ~7;
    }
    if (!found) {
        terminal_writestring("[FB] Tag framebuffer MB2 non trovato\n");
        return -1;
    }
    g_fb.addr  = found->addr;
    g_fb.pitch = found->pitch;
    g_fb.width = found->width;
    g_fb.height= found->height;
    g_fb.bpp   = found->bpp;
    g_fb.type  = found->type_fb;
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

    g_fb_ready = 1;
    return 0;
}

static inline uint32_t pack_rgb(uint8_t r,uint8_t g,uint8_t b){ return ((uint32_t)r<<16)|((uint32_t)g<<8)|((uint32_t)b); }

void fb_clear(uint32_t color) {
    if(!g_fb_ready) return;
    uint8_t* base = (uint8_t*)(g_fb.addr); // identity assumption
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
    uint8_t* base = (uint8_t*)(g_fb.addr);
    uint32_t* row = (uint32_t*)(base + y * g_fb.pitch);
    row[x] = color;
}

void fb_draw_test_pattern(void) {
    if(!g_fb_ready) return;
    // Gradient stripes
    for(int y=0;y<(int)g_fb.height;y++) {
        for(int x=0;x<(int)g_fb.width;x++) {
            uint8_t r = (uint8_t)((x * 255) / g_fb.width);
            uint8_t g = (uint8_t)((y * 255) / g_fb.height);
            uint8_t b = (uint8_t)((((x+y)/2) * 255) / ((g_fb.width+g_fb.height)/2));
            fb_putpixel(x,y, pack_rgb(r,g,b));
        }
    }
}

#endif // ENABLE_FB
