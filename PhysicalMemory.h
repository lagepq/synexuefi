#ifndef PHYSICAL_MEMORY_H
#define PHYSICAL_MEMORY_H

#include <Uefi.h>

// Initializes the custom page allocator with the provided memory range
void MemInit(EFI_PHYSICAL_ADDRESS BaseAddress, UINTN TotalPages);

// Allocates contiguous 4KB pages. Returns 0 if out of memory.
// All allocated pages are zero-initialized.
EFI_PHYSICAL_ADDRESS MemAllocatePages(UINTN NumberOfPages);
UINT64 TranslateGuestVirtual(UINT64 GuestCr3, UINT64 GuestVirtualAddress);

// (Optional) Free pages - for a bootkit, we rarely free memory once allocated
// void MemFreePages(EFI_PHYSICAL_ADDRESS Address, UINTN NumberOfPages);

#endif // PHYSICAL_MEMORY_H
