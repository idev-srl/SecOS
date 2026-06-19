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

// Graphics Output Protocol — exact UEFI spec layout (UEFI 2.9 §12.9)
typedef struct {
    uint32_t Version;               // offset 0
    uint32_t HorizontalResolution;  // offset 4
    uint32_t VerticalResolution;    // offset 8
    uint32_t PixelFormat;           // offset 12
    uint32_t PixelInformation[4];   // offset 16: EFI_PIXEL_BITMASK (Red/Green/Blue/Reserved masks)
    uint32_t PixelsPerScanLine;     // offset 32
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;                           // offset 0
    uint32_t Mode;                              // offset 4
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info; // offset 8
    uint64_t SizeOfInfo;                        // offset 16  (UINTN = 8 bytes on x64)
    uint64_t FrameBufferBase;                   // offset 24
    uint64_t FrameBufferSize;                   // offset 32
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL*, uint32_t, uint64_t*, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION**); // offset 0
    EFI_STATUS (EFIAPI *SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL*, uint32_t);  // offset 8
    void* Blt;                                  // offset 16 (not called; void* preserves layout)
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE* Mode;    // offset 24
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
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(uint32_t Type, uint32_t MemoryType, uint64_t Pages, uint64_t* Memory);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(uint32_t PoolType, uint64_t Size, void** Buffer);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(const EFI_GUID* Protocol, void* Registration, void** Interface);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle, uint64_t MapKey);

/* AllocatePages Type argument */
#define AllocateAnyPages 0

/*
 * EFI_BOOT_SERVICES — offsets per UEFI Spec (all pointers 8 bytes on x86-64):
 *   0   EFI_TABLE_HEADER (24 bytes)
 *  24   RaiseTPL
 *  32   RestoreTPL
 *  40   AllocatePages   ← first used field
 *  48   FreePages
 *  56   GetMemoryMap
 *  64   AllocatePool
 *  72   FreePool
 *  80   CreateEvent
 *  88   SetTimer
 *  96   WaitForEvent
 * 104   SignalEvent
 * 112   CloseEvent
 * 120   CheckEvent
 * 128   InstallProtocolInterface
 * 136   ReinstallProtocolInterface
 * 144   UninstallProtocolInterface
 * 152   HandleProtocol
 * 160   Reserved
 * 168   RegisterProtocolNotify
 * 176   LocateHandle
 * 184   LocateDevicePath
 * 192   InstallConfigurationTable
 * 200   LoadImage
 * 208   StartImage
 * 216   Exit
 * 224   UnloadImage
 * 232   ExitBootServices
 * 240..319  (10 × 8 bytes skipped)
 * 320   LocateProtocol
 */
typedef struct EFI_BOOT_SERVICES {
    char _pad[40];                              /* header(24)+RaiseTPL(8)+RestoreTPL(8) */
    EFI_ALLOCATE_PAGES    AllocatePages;        /* offset  40 */
    void*                 FreePages;            /* offset  48 */
    EFI_GET_MEMORY_MAP    GetMemoryMap;         /* offset  56 */
    EFI_ALLOCATE_POOL     AllocatePool;         /* offset  64 */
    void*                 FreePool;             /* offset  72 */
    void*                 CreateEvent;          /* offset  80 */
    void*                 SetTimer;             /* offset  88 */
    void*                 WaitForEvent;         /* offset  96 */
    void*                 SignalEvent;          /* offset 104 */
    void*                 CloseEvent;           /* offset 112 */
    void*                 CheckEvent;           /* offset 120 */
    void*                 InstallProtocolInterface;    /* 128 */
    void*                 ReinstallProtocolInterface;  /* 136 */
    void*                 UninstallProtocolInterface;  /* 144 */
    void*                 HandleProtocol;              /* 152 */
    void*                 _reserved;                   /* 160 */
    void*                 RegisterProtocolNotify;      /* 168 */
    void*                 LocateHandle;                /* 176 */
    void*                 LocateDevicePath;             /* 184 */
    void*                 InstallConfigurationTable;   /* 192 */
    void*                 LoadImage;                   /* 200 */
    void*                 StartImage;                  /* 208 */
    void*                 Exit;                        /* 216 */
    void*                 UnloadImage;                 /* 224 */
    EFI_EXIT_BOOT_SERVICES ExitBootServices;           /* offset 232 */
    char _pad2[80];                             /* 240-319: 10 skipped entries */
    EFI_LOCATE_PROTOCOL   LocateProtocol;              /* offset 320 */
} EFI_BOOT_SERVICES;

/*
 * EFI_SYSTEM_TABLE — offsets per UEFI Spec:
 *   0   EFI_TABLE_HEADER (24 bytes)
 *  24   FirmwareVendor (CHAR16*)
 *  32   FirmwareRevision (UINT32)
 *  36   [4 bytes implicit padding]
 *  40   ConsoleInHandle (EFI_HANDLE)
 *  48   ConIn
 *  56   ConsoleOutHandle (EFI_HANDLE)
 *  64   ConOut
 *  72   StdErrHandle (EFI_HANDLE)
 *  80   StdErr
 *  88   RuntimeServices
 *  96   BootServices
 */
typedef struct EFI_SYSTEM_TABLE {
    char     _pad_header[24];                        /* EFI_TABLE_HEADER */
    CHAR16*  FirmwareVendor;                         /* offset  24 */
    uint32_t FirmwareRevision;                       /* offset  32 */
    /* 4 bytes implicit padding to align next pointer */
    EFI_HANDLE                       ConsoleInHandle; /* offset  40 */
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL*  ConIn;           /* offset  48 */
    EFI_HANDLE                       ConsoleOutHandle;/* offset  56 */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut;          /* offset  64 */
    EFI_HANDLE                       StdErrHandle;    /* offset  72 */
    void*                            StdErr;          /* offset  80 */
    void*                            RuntimeServices; /* offset  88 */
    EFI_BOOT_SERVICES*               BootServices;    /* offset  96 */
    uint64_t                         NumberOfTableEntries; /* offset 104 */
    struct EFI_CONFIGURATION_TABLE*  ConfigurationTable;   /* offset 112 */
} EFI_SYSTEM_TABLE;

/* [M28] EFI configuration table entry — carries the ACPI RSDP among others. */
typedef struct EFI_CONFIGURATION_TABLE {
    EFI_GUID VendorGuid;
    void*    VendorTable;
} EFI_CONFIGURATION_TABLE;

/* ACPI 2.0+ and legacy ACPI 1.0 RSDP configuration-table GUIDs. */
static const EFI_GUID EFI_ACPI_20_TABLE_GUID = {0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};
static const EFI_GUID EFI_ACPI_10_TABLE_GUID = {0xeb9d2d30,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

// Status codes (subset)
// Status codes: high bit set for error conditions per UEFI spec.
#define EFI_SUCCESS 0ULL
#define EFI_ERROR_BIT 0x8000000000000000ULL
#define EFI_INVALID_PARAMETER (EFI_ERROR_BIT | 2ULL)
#define EFI_UNSUPPORTED       (EFI_ERROR_BIT | 3ULL)

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
    uint64_t   Revision;                                                                                    /* offset  0 */
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*, struct EFI_FILE_PROTOCOL**);  /* offset  8 */
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL {
    uint64_t   Revision;                                                                                    /* offset  0 */
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE_PROTOCOL*, struct EFI_FILE_PROTOCOL**, const CHAR16* FileName, uint64_t OpenMode, uint64_t Attributes); /* offset  8 */
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE_PROTOCOL*);                                                 /* offset 16 */
    void*      Delete;                                                                                      /* offset 24 */
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE_PROTOCOL*, uint64_t* BufferSize, void* Buffer);              /* offset 32 */
    // Additional methods omitted.
} EFI_FILE_PROTOCOL;

// Protocol GUIDs (GOP)
static const EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID = {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
// NOTE: GUIDs below should be verified against the UEFI specification; provided values are placeholders that may need adjustment.
static const EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};


// Helper to make a CHAR16 literal (wide) from ASCII (simple macro for short strings)
#define WIDE(str) ((const CHAR16*)L"" str)

#endif // SECOS_EFI_H

// --- SecOS estensioni interne per loader ELF ---
// Max number of loadable segments tracked (must match elf_load.c definition)
#ifndef SECOS_MAX_SEGMENTS
#define SECOS_MAX_SEGMENTS 16
#endif
typedef struct {
    uint64_t vaddr;       // Indirizzo virtuale target (higher-half per il kernel)
    uint64_t paddr;       // [M12] Indirizzo fisico (LMA) dove caricare il segmento
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
