#ifndef SECOS_EFI_H
#define SECOS_EFI_H
/*
 * SecOS UEFI Bootloader Support Header
 * Minimal UEFI type & protocol definitions (Strategy B external loader).
 * Provides enough surface for: console output, memory map, GOP framebuffer, simple
 * filesystem access (Simple File System + File Protocol) and ELF loading.
 * This is intentionally partial; many fields are omitted for brevity.
 *
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

// Basic types
typedef uint64_t EFI_STATUS;
typedef void*    EFI_HANDLE;
typedef uint16_t CHAR16;      // with -fshort-wchar

// Calling convention: UEFI x86_64 usa Microsoft ABI. Forziamo ms_abi per compatibilità.
#if defined(__GNUC__) || defined(__clang__)
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif

typedef struct {
    uint32_t Type;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

// Table forward declarations
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_BOOT_SERVICES;
struct EFI_RUNTIME_SERVICES;

// GUID structure
typedef struct { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; } EFI_GUID;

// Graphics Output Protocol
typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelFormat; // Simplified
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info;
    uint32_t SizeOfInfo;
    uint32_t Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL*, uint32_t, uint64_t*, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION**);
    EFI_STATUS (EFIAPI *SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL*, uint32_t);
    // Blt omitted
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE* Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

// Simple Text Output
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, int Extended);
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, const CHAR16* String);
    // Other members omitted
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

// Simple Text Input (minimal)
typedef struct {
    uint16_t ScanCode;
    CHAR16   UnicodeChar;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, int Extended);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, EFI_INPUT_KEY* Key);
    void* WaitForKey; // EVENT*
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

// Boot Services subset
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(uint64_t* MemoryMapSize, EFI_MEMORY_DESCRIPTOR* MemoryMap, uint64_t* MapKey, uint64_t* DescriptorSize, uint32_t* DescriptorVersion);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(uint32_t PoolType, uint64_t Size, void** Buffer);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(const EFI_GUID* Protocol, void* Registration, void** Interface);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle, uint64_t MapKey);

typedef struct EFI_BOOT_SERVICES {
    char _pad[24]; // Skip first fields (RaiseTPL/RestoreTPL etc.)
    EFI_GET_MEMORY_MAP    GetMemoryMap;
    EFI_ALLOCATE_POOL     AllocatePool;
    void*                 FreePool; // not used
    void*                 CreateEvent; // omitted
    void*                 SetTimer;    // omitted
    void*                 WaitForEvent; // omitted
    void*                 SignalEvent; // omitted
    void*                 CloseEvent;  // omitted
    EFI_LOCATE_PROTOCOL   LocateProtocol;
    char _pad2[56]; // skip to ExitBootServices (layout simplified)
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
} EFI_BOOT_SERVICES;

// System Table
typedef struct EFI_SYSTEM_TABLE {
    char _pad_header[24]; // Signature, Revision, CRC32, etc.
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL*  ConIn;
    EFI_BOOT_SERVICES* BootServices;
    void* RuntimeServices; // placeholder (unused)
} EFI_SYSTEM_TABLE;

// Status codes (subset)
// Status codes: high bit set for error conditions per UEFI spec.
#define EFI_SUCCESS 0ULL
#define EFI_ERROR_BIT 0x8000000000000000ULL
#define EFI_UNSUPPORTED (EFI_ERROR_BIT | 3ULL)

// Common error (generic) macro (simplified)
#define EFI_ERR(x) ((EFI_STATUS)(EFI_ERROR_BIT | (uint64_t)(x)))

// Pool types (subset)
#define EFI_LOADER_DATA 2

// File open modes
#define EFI_FILE_MODE_READ   0x0000000000000001ULL
// File attributes (unused for read-only)

// Forward declarations for File Protocol
struct EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    EFI_STATUS (*OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*, struct EFI_FILE_PROTOCOL**);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL {
    EFI_STATUS (*Open)(struct EFI_FILE_PROTOCOL*, struct EFI_FILE_PROTOCOL**, const CHAR16* FileName, uint64_t OpenMode, uint64_t Attributes);
    EFI_STATUS (*Close)(struct EFI_FILE_PROTOCOL*);
    EFI_STATUS (*Read)(struct EFI_FILE_PROTOCOL*, uint64_t* BufferSize, void* Buffer);
    // Additional methods omitted.
} EFI_FILE_PROTOCOL;

// Protocol GUIDs (GOP)
static const EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID = {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
// NOTE: GUIDs below should be verified against the UEFI specification; provided values are placeholders that may need adjustment.
static const EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {0x0964e5b6,0x5b22,0x4e5d,{0x8e,0x65,0x42,0x3b,0xb6,0x8c,0x53,0x54}}; // Placeholder GUID


// Helper to make a CHAR16 literal (wide) from ASCII (simple macro for short strings)
#define WIDE(str) ((const CHAR16*)L"" str)

#endif // SECOS_EFI_H

// --- SecOS estensioni interne per loader ELF ---
// Max number of loadable segments tracked (must match elf_load.c definition)
#ifndef SECOS_MAX_SEGMENTS
#define SECOS_MAX_SEGMENTS 16
#endif
typedef struct {
    uint64_t vaddr;       // Indirizzo virtuale target
    uint64_t memsz;       // Dimensione richiesta in memoria
    uint64_t filesz;      // Dimensione effettiva nel file
    uint32_t flags;       // Flag ELF (PF_X=1 PF_W=2 PF_R=4)
    uint8_t* data;        // Buffer temporaneo con contenuto segment (pool UEFI)
} secos_loaded_segment_t;

extern secos_loaded_segment_t g_loaded_segments[];       // Array segmenti caricati
extern uint16_t               g_loaded_segment_count;    // Numero segmenti
extern void*                  g_kernel_entry;            // Entry point ELF

// --- W^X Remap Planning (design-only for now) ---
// We will later rebuild page tables with 4KB granularity assigning permissions:
//  - CODE_RX: executable + read, no write
//  - DATA_RW: writable data (and read), no execute
//  - RODATA_R: read-only, no write, no execute
// Classification helper enum:
typedef enum {
    SECOS_SEG_CLASS_CODE_RX = 1,
    SECOS_SEG_CLASS_DATA_RW = 2,
    SECOS_SEG_CLASS_RODATA_R = 3
} secos_segment_class_t;

static inline secos_segment_class_t secos_classify_segment(uint32_t flags) {
    int exec = !!(flags & 1);   // PF_X
    int write= !!(flags & 2);   // PF_W
    int read = !!(flags & 4);   // PF_R
    if (exec && read && !write) return SECOS_SEG_CLASS_CODE_RX;
    if (!exec && write) return SECOS_SEG_CLASS_DATA_RW; // Assume readable
    if (!exec && !write && read) return SECOS_SEG_CLASS_RODATA_R;
    // Unexpected combos (e.g., X+W) fallback to DATA_RW but lose execute to avoid RWX.
    if (exec && write) return SECOS_SEG_CLASS_DATA_RW;
    return SECOS_SEG_CLASS_DATA_RW;
}

typedef struct {
    uint64_t vaddr_start;  // 4KB aligned start
    uint64_t page_count;   // number of 4KB pages
    secos_segment_class_t cls; // permission class
} secos_final_mapping_t;

#define SECOS_MAX_FINAL_MAPPINGS SECOS_MAX_SEGMENTS
extern secos_final_mapping_t g_final_mappings[SECOS_MAX_FINAL_MAPPINGS];
extern uint16_t g_final_mapping_count;
