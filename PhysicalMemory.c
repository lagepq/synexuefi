#include "PhysicalMemory.h"
#include <Library/BaseMemoryLib.h>

static EFI_PHYSICAL_ADDRESS g_MemBase = 0;
static UINTN g_MemTotalPages = 0;
static UINTN g_MemAllocatedPages = 0;

void MemInit(EFI_PHYSICAL_ADDRESS BaseAddress, UINTN TotalPages)
{
    g_MemBase = BaseAddress;
    g_MemTotalPages = TotalPages;
    g_MemAllocatedPages = 0;
    
    // Zero out the entire memory region for safety
    ZeroMem((VOID*)g_MemBase, TotalPages * EFI_PAGE_SIZE);
}

EFI_PHYSICAL_ADDRESS MemAllocatePages(UINTN NumberOfPages)
{
    if (g_MemAllocatedPages + NumberOfPages > g_MemTotalPages) {
        return 0; // Out of memory
    }
    
    EFI_PHYSICAL_ADDRESS AllocatedAddress = g_MemBase + (g_MemAllocatedPages * EFI_PAGE_SIZE);
    g_MemAllocatedPages += NumberOfPages;
    
    return AllocatedAddress;
}
