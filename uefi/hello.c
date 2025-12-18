// Minimal UEFI Hello Loader
#include "efi.h"

// Simple print helper (relies on ConOut->OutputString)
static void puts(EFI_SYSTEM_TABLE *st, const CHAR16 *s) {
    st->ConOut->OutputString(st->ConOut, (CHAR16*)s);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    puts(st, L"Hello from minimal UEFI loader\r\n");
    // Wait for key
    EFI_INPUT_KEY key;
    puts(st, L"Press any key to exit...\r\n");
    st->ConIn->ReadKeyStroke(st->ConIn, &key);
    return EFI_SUCCESS;
}
