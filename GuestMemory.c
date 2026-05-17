#include <Uefi.h>
#include <intrin.h>
#include "GuestRegisters.h"

// Helper to walk guest page tables (x64 4-level paging)
// Returns 0 on failure or the Guest Physical Address (GPA)
UINT64 TranslateGuestVirtual(UINT64 GuestCr3, UINT64 GuestVirtualAddress)
{
    UINT64 Pml4Index = (GuestVirtualAddress >> 39) & 0x1FF;
    UINT64 PdptIndex = (GuestVirtualAddress >> 30) & 0x1FF;
    UINT64 PdIndex   = (GuestVirtualAddress >> 21) & 0x1FF;
    UINT64 PtIndex   = (GuestVirtualAddress >> 12) & 0x1FF;
    UINT64 Offset    = GuestVirtualAddress & 0xFFF;

    UINT64 PhysMask = 0x000FFFFFFFFFF000ULL; // Standard mask for 4KB alignment in 64-bit

    // 1. PML4
    UINT64* Pml4 = (UINT64*)(GuestCr3 & PhysMask);
    UINT64 Pml4e = Pml4[Pml4Index];
    if (!(Pml4e & 1)) return 0; // Not present

    // 2. PDPT
    UINT64* Pdpt = (UINT64*)(Pml4e & PhysMask);
    UINT64 Pdpte = Pdpt[PdptIndex];
    if (!(Pdpte & 1)) return 0; // Not present
    
    // Check for 1GB huge page
    if (Pdpte & 0x80) {
        return (Pdpte & 0xFFFFFC0000000ULL) + (GuestVirtualAddress & 0x3FFFFFFF);
    }

    // 3. PD
    UINT64* Pd = (UINT64*)(Pdpte & PhysMask);
    UINT64 Pde = Pd[PdIndex];
    if (!(Pde & 1)) return 0; // Not present
    
    // Check for 2MB large page
    if (Pde & 0x80) {
        return (Pde & 0xFFFFFFFE00000ULL) + (GuestVirtualAddress & 0x1FFFFF);
    }

    // 4. PT
    UINT64* Pt = (UINT64*)(Pde & PhysMask);
    UINT64 Pte = Pt[PtIndex];
    if (!(Pte & 1)) return 0; // Not present

    return (Pte & PhysMask) + Offset;
}
