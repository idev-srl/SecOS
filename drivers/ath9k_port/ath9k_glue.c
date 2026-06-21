/*
 * SecOS - glue between the upstream ath9k_hw driver (ported as-is via the
 * compat shim) and the SecOS kernel. Provides the ~40 Linux kernel-API symbols
 * the ath9k_hw object set references at link time, mapped to SecOS primitives or
 * minimal stubs (ANI/dynack/regd/btcoex are not part of basic bring-up).
 * SPDX-License-Identifier: MIT
 */
#include <linux/types.h>
#include <linux/slab.h>

/* SecOS kernel primitives (already in kernel.bin: kmalloc/kfree/memcpy/memset
 * resolve directly at link — the shim's 2-arg kmalloc proto is ABI-compatible
 * with SecOS's 1-arg kmalloc, x86-64 SysV ignores the extra reg). */
extern void  debugcon_writestring(const char*);
extern unsigned long timer_get_ticks(void);
extern void* memset(void*, int, unsigned long);

/* --- heap: only kzalloc is missing on the SecOS side (kmalloc from slab.h) --- */
void* kzalloc(unsigned long sz, unsigned f){ void* p=kmalloc(sz, f); if(p) memset(p,0,sz); return p; }

/* --- time --- */
unsigned long jiffies = 0;
s64 ktime_get_raw(void){ return (s64)timer_get_ticks() * 1000000; }   /* ns from ms ticks */
s64 ktime_us_delta(s64 a, s64 b){ return (a - b) / 1000; }
s64 ktime_get(void){ return ktime_get_raw(); }

/* --- atomics (single-core driver context) --- */
int  atomic_read(const atomic_t* v){ return v->counter; }
void atomic_set(atomic_t* v, int i){ v->counter = i; }
void atomic_inc(atomic_t* v){ v->counter++; }
void atomic_dec(atomic_t* v){ v->counter--; }
int  atomic_inc_and_test(atomic_t* v){ return ++v->counter == 0; }
int  atomic_dec_and_test(atomic_t* v){ return --v->counter == 0; }

/* --- string/format helpers --- */
int scnprintf(char* buf, unsigned long sz, const char* fmt, ...){ (void)fmt; if(sz) buf[0]=0; return 0; }
int snprintf(char* buf, unsigned long sz, const char* fmt, ...){ (void)fmt; if(sz) buf[0]=0; return 0; }
u16 swab16(u16 x){ return __builtin_bswap16(x); }
u32 swab32(u32 x){ return __builtin_bswap32(x); }
s32 sign_extend32(u32 v, int idx){ int s = 31 - idx; return (s32)(v << s) >> s; }
void sort(void* base, unsigned long n, unsigned long sz, int(*cmp)(const void*,const void*), void* x){ (void)base;(void)n;(void)sz;(void)cmp;(void)x; }

/* --- ethernet helpers --- */
int is_valid_ether_addr(const u8* a){ return !(a[0]&1) && (a[0]|a[1]|a[2]|a[3]|a[4]|a[5]); }
int is_zero_ether_addr(const u8* a){ return !(a[0]|a[1]|a[2]|a[3]|a[4]|a[5]); }
void eth_random_addr(u8* a){ for(int i=0;i<6;i++) a[i]=0; a[0]=0x02; }
void eth_broadcast_addr(u8* a){ for(int i=0;i<6;i++) a[i]=0xff; }

/* --- ath9k common (common.c/regd.c not ported; safe defaults) --- */
void ath_printk(const char* lvl, const void* c, const char* fmt, ...){ (void)lvl;(void)c;(void)fmt; }
void ath_hw_setbssidmask(void* common){ (void)common; }
unsigned ath_regd_get_band_ctl(void* reg, int band){ (void)reg;(void)band; return 0; /* SD_NO_CTL */ }
int ath_hw_get_listen_time(void* common){ (void)common; return 0; }
void dev_err(const void* d, const char* fmt, ...){ (void)d;(void)fmt; }
void dev_warn(const void* d, const char* fmt, ...){ (void)d;(void)fmt; }
void dev_dbg(const void* d, const char* fmt, ...){ (void)d;(void)fmt; }

/* --- GPIO (LED/rfkill; not used for bring-up) --- */
void* gpiod_get_index(void* d, const char* c, unsigned i, int f){ (void)d;(void)c;(void)i;(void)f; return 0; }
void  gpiod_put(void* d){ (void)d; }
void  gpiod_set_consumer_name(void* d, const char* n){ (void)d;(void)n; }

/* --- ANI / dynack (not part of basic bring-up) --- */
void ath9k_ani_reset(void* ah, int is_scanning){ (void)ah;(void)is_scanning; }
void ath9k_enable_mib_counters(void* ah){ (void)ah; }
void ath9k_hw_ani_monitor(void* ah, void* chan){ (void)ah;(void)chan; }
void ath_dynack_reset(void* ah){ (void)ah; }
void ath_dynack_init(void* ah){ (void)ah; }

/* --- EEPROM ops tables for OTHER chip families (AR9565 uses eep_ar9300_ops). --- */
void* eep_def_ops = 0;
void* eep_4k_ops = 0;
void* eep_ar9287_ops = 0;

/* --- AR9002 (old chip family) + optional features: referenced by hw.c's chip
 * dispatch but never called for the AR9565 (AR9300). Safe stubs. --- */
int  ar9002_hw_attach_ops(void* ah){ (void)ah; return 0; }
void ar9002_hw_enable_async_fifo(void* ah){ (void)ah; }
void ar9002_hw_load_ani_reg(void* ah, void* chan){ (void)ah;(void)chan; }
int  ar9002_hw_rf_claim(void* ah){ (void)ah; return 0; }
int  ar9003_is_paprd_enabled(void* ah){ (void)ah; return 0; }
void ath9k_hw_ani_init(void* ah){ (void)ah; }
void ath9k_hw_disable_mib_counters(void* ah){ (void)ah; }

/* --- mem* as global symbols (SecOS's are inline-only; the ported driver needs
 * real symbols). gcc may also lower struct copies to these. --- */
void* memcpy(void* d, const void* s, unsigned long n){ unsigned char* a=d; const unsigned char* b=s; while(n--) *a++=*b++; return d; }
void* memset(void* d, int c, unsigned long n){ unsigned char* a=d; while(n--) *a++=(unsigned char)c; return d; }
void* memmove(void* d, const void* s, unsigned long n){ unsigned char* a=d; const unsigned char* b=s; if(a<b){while(n--)*a++=*b++;}else{a+=n;b+=n;while(n--)*--a=*--b;} return d; }
int memcmp(const void* a, const void* b, unsigned long n){ const unsigned char* x=a;const unsigned char* y=b; for(unsigned long i=0;i<n;i++){if(x[i]!=y[i])return (int)x[i]-(int)y[i];} return 0; }
unsigned long strlen(const char* s){ unsigned long n=0; while(s[n])n++; return n; }

/* --- debugcon (SecOS's are static-inline; provide globals for the bridge) --- */
static void e9(char c){ __asm__ volatile("outb %0, %1" :: "a"((unsigned char)c), "Nd"((unsigned short)0xE9)); }
void debugcon_putchar(char c){ e9(c); }
void debugcon_writestring(const char* s){ while(*s) e9(*s++); }
void debugcon_print_hex(unsigned long v){ static const char h[]="0123456789ABCDEF"; e9('0'); e9('x'); for(int i=60;i>=0;i-=4) e9(h[(v>>i)&0xF]); }
