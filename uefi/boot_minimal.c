// UEFI bootloader - loads kernel.elf and passes UEFI memory map

#include <efi.h>
#include <efilib.h>
#include <stdint.h>

// Boot info structure (from secos kernel/bootinfo.h)
struct secos_boot_info {
    uint64_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;
    uint64_t mem_desc_count;
    void*    mem_descs;
    uint64_t mem_desc_size;
    uint64_t mem_desc_version;
    uint64_t flags;
} __attribute__((packed));

// ELF64 header and program header
typedef struct {
    unsigned char e_ident[16];
    unsigned short e_type;
    unsigned short e_machine;
    unsigned int e_version;
    unsigned long e_entry;
    unsigned long e_phoff;
    unsigned long e_shoff;
    unsigned int e_flags;
    unsigned short e_ehsize;
    unsigned short e_phentsize;
    unsigned short e_phnum;
    unsigned short e_shentsize;
    unsigned short e_shnum;
    unsigned short e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    unsigned int p_type;
    unsigned int p_flags;
    unsigned long p_offset;
    unsigned long p_vaddr;
    unsigned long p_paddr;
    unsigned long p_filesz;
    unsigned long p_memsz;
    unsigned long p_align;
} Elf64_Phdr;

#define PT_LOAD 1

// Disable UEFI paging before jumping to kernel (important!)
// The kernel will setup its own page tables in long mode
static void disable_uefi_paging(void) {
    // This function would need to be in assembly to properly disable paging
    // For now, we document that UEFI has paging enabled with a memory map
    // The kernel's _start must handle this by immediately disabling paging
    // and clearing CR3 to avoid conflicts
}

// Copy kernel ELF segments to their target virtual addresses
// For UEFI boot, we assume identity address mapping (phys = virt)
static UINT64 copy_kernel_segments(UINT8 *file_buf, UINT64 file_size) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr*)file_buf;
    UINT64 max_vaddr = 0;
    
    // First pass: check where kernel segments are and how much space needed
    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *phdr = (Elf64_Phdr*)((UINT8*)file_buf + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD) continue;
        
        UINT64 end = phdr->p_vaddr + phdr->p_memsz;
        if (end > max_vaddr) max_vaddr = end;
    }
    
    return ehdr->e_entry;
}

static struct secos_boot_info* prepare_boot_info(void) {
    EFI_STATUS status;
    struct secos_boot_info *bi;
    void *mmap_buf = NULL;
    UINTN mmap_size = 0, mmap_key, desc_size;
    UINT32 desc_version;
    
    Print(L"Getting UEFI memory map...\r\n");
    
    // Query size
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &mmap_size, NULL, &mmap_key, &desc_size, &desc_version);
    if (status != EFI_BUFFER_TOO_SMALL) {
        Print(L"GetMemoryMap size query failed: %x\r\n", status);
        return NULL;
    }
    
    // Add buffer for safety
    mmap_size += 2 * desc_size;
    
    // Alloca per memory map
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, mmap_size, &mmap_buf);
    if (EFI_ERROR(status)) {
        Print(L"Cannot allocate mmap buffer: %x\r\n", status);
        return NULL;
    }
    
    // Get actual map
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &mmap_size, mmap_buf, &mmap_key, &desc_size, &desc_version);
    if (EFI_ERROR(status)) {
        Print(L"GetMemoryMap failed: %x\r\n", status);
        return NULL;
    }
    
    // Count descriptors
    uint64_t desc_count = mmap_size / desc_size;
    Print(L"Memory map: %ld descriptors, size=%ld bytes\r\n", desc_count, desc_size);
    
    // Alloca secos_boot_info
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, sizeof(struct secos_boot_info), (void**)&bi);
    if (EFI_ERROR(status)) {
        Print(L"Cannot allocate boot info: %x\r\n", status);
        return NULL;
    }
    
    // Popola boot info
    bi->fb_addr = 0;
    bi->fb_width = 0;
    bi->fb_height = 0;
    bi->fb_pitch = 0;
    bi->fb_bpp = 0;
    bi->mem_descs = mmap_buf;
    bi->mem_desc_count = desc_count;
    bi->mem_desc_size = desc_size;
    bi->mem_desc_version = desc_version;
    bi->flags = (1ULL << 1); // bit1: memory map valid
    
    // Try to get GOP framebuffer
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    status = uefi_call_wrapper(BS->LocateProtocol, 3, &gEfiGraphicsOutputProtocolGuid, NULL, (void**)&gop);
    if (!EFI_ERROR(status) && gop && gop->Mode && gop->Mode->FrameBufferBase) {
        bi->fb_addr = gop->Mode->FrameBufferBase;
        bi->fb_width = gop->Mode->Info->HorizontalResolution;
        bi->fb_height = gop->Mode->Info->VerticalResolution;
        bi->fb_pitch = gop->Mode->Info->PixelsPerScanLine * 4;
        bi->fb_bpp = 32;
        bi->flags |= (1ULL << 0);  // bit0: GOP valid
        Print(L"GOP Framebuffer: 0x%x (%dx%d)\r\n", bi->fb_addr, bi->fb_width, bi->fb_height);
    } else {
        Print(L"GOP not available\r\n");
    }
    
    
    Print(L"Boot info prepared at 0x%x\r\n", (UINT64)bi);
    return bi;
}

static void* load_kernel(EFI_FILE *root) {
    EFI_FILE *file = NULL;
    EFI_STATUS status;
    void *file_buf = NULL;
    UINT64 file_size = 512*1024;
    
    Print(L"Loading kernel.elf...\r\n");
    
    if (!root) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *vol = NULL;
        status = uefi_call_wrapper(BS->LocateProtocol, 3, &gEfiSimpleFileSystemProtocolGuid, NULL, (void**)&vol);
        if (EFI_ERROR(status)) {
            Print(L"No filesystem\r\n");
            return NULL;
        }
        status = uefi_call_wrapper(vol->OpenVolume, 2, vol, &root);
        if (EFI_ERROR(status)) {
            Print(L"Cannot open volume\r\n");
            return NULL;
        }
    }
    
    status = uefi_call_wrapper(root->Open, 5, root, &file, L"kernel.elf", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        Print(L"Cannot open kernel.elf: %x\r\n", status);
        return NULL;
    }
    
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, file_size, &file_buf);
    if (EFI_ERROR(status)) {
        Print(L"Cannot allocate: %x\r\n", status);
        return NULL;
    }
    
    status = uefi_call_wrapper(file->Read, 3, file, &file_size, file_buf);
    if (EFI_ERROR(status)) {
        Print(L"Cannot read: %x\r\n", status);
        return NULL;
    }
    
    uefi_call_wrapper(file->Close, 1, file);
    Print(L"Kernel ELF file loaded: %ld bytes at 0x%x\r\n", file_size, (UINT64)file_buf);
    
    // Parse ELF header
    Elf64_Ehdr *ehdr = (Elf64_Ehdr*)file_buf;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E') {
        Print(L"Not an ELF file\r\n");
        return NULL;
    }
    
    Print(L"ELF: %d program headers, entry=0x%x\r\n", ehdr->e_phnum, ehdr->e_entry);
    
    // Load only PT_LOAD segments to their correct virtual addresses
    // Each segment is placed at its vaddr (which equals paddr for kernel)
    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *phdr = (Elf64_Phdr*)((UINT8*)file_buf + ehdr->e_phoff + i * ehdr->e_phentsize);
        
        if (phdr->p_type != PT_LOAD) continue;
        
        void *src = (UINT8*)file_buf + phdr->p_offset;
        void *dst = (void*)(UINT64)phdr->p_vaddr;  // Kernel vaddr = physical for identity map
        
        Print(L"  Segment %d: 0x%x <- 0x%x (%ld bytes)\r\n", 
              i, (UINT64)dst, (UINT64)src, phdr->p_filesz);
        
        // Copy file content
        if (phdr->p_filesz > 0) {
            for (UINT64 j = 0; j < phdr->p_filesz; j++) {
                ((UINT8*)dst)[j] = ((UINT8*)src)[j];
            }
        }
        
        // Zero BSS
        if (phdr->p_memsz > phdr->p_filesz) {
            UINT64 bss_size = phdr->p_memsz - phdr->p_filesz;
            for (UINT64 j = 0; j < bss_size; j++) {
                ((UINT8*)dst)[phdr->p_filesz + j] = 0;
            }
            Print(L"    BSS: %ld bytes zeroed\r\n", bss_size);
        }
    }
    
    Print(L"Kernel segments loaded\r\n");
    
    // Store entry point BEFORE freeing buffer
    UINT64 entry = ehdr->e_entry;
    Print(L"Kernel entry point: 0x%x\r\n", entry);
    
    // Free the file buffer
    uefi_call_wrapper(BS->FreePool, 1, file_buf);
    
    return (void*)entry;
}


EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    
    Print(L"\n\rSecOS UEFI Bootloader (gnu-efi)\r\n");
    Print(L"=====================================\r\n");
    
    // Prepare boot info with memory map
    struct secos_boot_info *boot_info = prepare_boot_info();
    if (!boot_info) {
        Print(L"FAILED TO PREPARE BOOT INFO\r\n");
        goto fail;
    }
    
    // Carica kernel
    void *entry = load_kernel(NULL);
    if (!entry) {
        Print(L"FAILED TO LOAD KERNEL\r\n");
        goto fail;
    }
    
    Print(L"Executing kernel at 0x%x\r\n", (UINT64)entry);
    Print(L"Boot info at 0x%x\r\n", (UINT64)boot_info);
    Print(L"\r\n");
    Print(L"Disabling paging and jumping to kernel...\r\n");
    Print(L"\r\n");
    
    // Disable paging, clear CR3, and jump to kernel entry
    // This is critical: kernel expects paging to be disabled
    __asm__ __volatile__(
        "cli\n\t"              // Disable interrupts
        "mov $0x0, %%rax\n\t"  // magic = 0 (UEFI boot)
        "mov %0, %%rsi\n\t"    // info = boot_info in RSI
        // Disable paging
        "mov %%cr0, %%rcx\n\t"
        "btr $31, %%rcx\n\t"   // Clear CR0.PG (bit 31)
        "mov %%rcx, %%cr0\n\t"
        "xor %%rcx, %%rcx\n\t"
        "mov %%rcx, %%cr3\n\t" // Clear CR3
        // Jump to kernel entry point
        "jmp *%1\n\t"
        : : "r"((UINT64)boot_info), "r"((UINT64)entry) : "rax", "rsi", "rcx"
    );
    
    // Unreachable
    goto fail;
    
fail:
    Print(L"Returned from kernel (unexpected!)\r\n");
    Print(L"Press any key to reboot...\r\n");
    
    EFI_INPUT_KEY key;
    uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
    uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
    
    return EFI_SUCCESS;
}
