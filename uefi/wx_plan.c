/*
 * W^X Remap Plan Stub
 * This file outlines the future implementation steps for rebuilding page tables
 * with 4KB granularity enforcing write-or-execute (never both) on kernel segments.
 *
 * Strategy:
 * 1. Classify loaded ELF segments (already stored in g_loaded_segments) using secos_classify_segment.
 * 2. For each segment, break its [vaddr, vaddr+memsz) range into 4KB pages.
 * 3. Populate g_final_mappings with one entry per contiguous classification block.
 * 4. Allocate new page tables (PML4/PDPT/PDT/PT) distinct from current identity 2MB layout.
 * 5. Map CODE_RX pages with present|read|exec (clear write), DATA_RW with present|read|write (NX), RODATA_R with present|read (NX).
 * 6. Copy segment data from temporary pool buffers to their final physical pages.
 * 7. Zero temporary pool buffers and mark them free.
 * 8. Mark page table pages read-only (clear write bit) after setup.
 * 9. Install new CR3 and flush TLB, then continue boot flow (ExitBootServices already performed earlier).
 *
 * Security Additions:
 * - Insert a guard page (unmapped) below and above initial kernel stack base.
 * - Optionally randomize stack offset within its page to mitigate simple exploits.
 * - Filter out UEFI runtime service regions from physical allocator before mapping.
 *
 * NOTE: This is design-only; no executable code yet to avoid partial/unsafe state.
 */

#include "efi.h"

secos_final_mapping_t g_final_mappings[SECOS_MAX_FINAL_MAPPINGS];
uint16_t g_final_mapping_count = 0;

// Future function stub: build final mapping plan
void secos_build_wx_plan(void) {
    // Placeholder: iterate g_loaded_segments and classify.
    // for (uint16_t i=0; i<g_loaded_segment_count; ++i) { ... }
}
