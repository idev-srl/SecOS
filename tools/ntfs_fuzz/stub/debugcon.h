/* Host stub of lib/debugcon.h for the NTFS fuzz harness. */
#pragma once
#include <stdint.h>
#include <stdio.h>
extern int g_fuzz_verbose;
static inline void debugcon_writestring(const char* s){ if(g_fuzz_verbose) fputs(s, stderr); }
static inline void debugcon_print_hex(uint64_t v){ if(g_fuzz_verbose) fprintf(stderr, "0x%016llX", (unsigned long long)v); }
