/** @file
  SecOS UEFI Bootloader using EDK2

  Copyright (c) 2025, SecOS. All rights reserved.
  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  Print(L"SecOS UEFI Bootloader - EDK2 Version\n");
  Print(L"Press any key to continue...\n");
  
  // Wait for key
  EFI_INPUT_KEY Key;
  gST->ConIn->Reset(gST->ConIn, FALSE);
  gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
  
  Print(L"Bootloader executed successfully!\n");
  return EFI_SUCCESS;
}
