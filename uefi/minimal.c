// Minimal UEFI bootloader - no dependencies
typedef unsigned short CHAR16;
typedef unsigned long long UINT64;
typedef unsigned long UINTN;
typedef long long EFI_STATUS;
typedef void* EFI_HANDLE;

#define EFI_SUCCESS 0

typedef struct {
    UINT64 _pad[32];
    struct {
        void* _pad1[4];
        EFI_STATUS (*OutputString)(void*, CHAR16*);
    } *ConOut;
} EFI_SYSTEM_TABLE;

__attribute__((ms_abi))
EFI_STATUS efi_main(EFI_HANDLE h, EFI_SYSTEM_TABLE *st) {
    st->ConOut->OutputString(st->ConOut, L"SecOS Minimal UEFI Boot!\r\n");
    // Infinite loop
    while(1) { __asm__ volatile("hlt"); }
    return EFI_SUCCESS;
}
