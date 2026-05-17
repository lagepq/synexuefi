#include "Ept.h"
#include "PhysicalMemory.h"
#include "ComLogger.h"
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <intrin.h>


// Global structures
EPTP g_EptPointer = {0};
EPTHOOK_ENTRY g_EptHooks[MAX_EPT_HOOKS] = {0};

EPTP InitializeEpt(VOID)
{
    EPTP EptPointer = {0};

    // Build a 512GB identity map using 2MB large pages.
    //
    // Layout:  PML4[0] -> PDPT -> PD[0..511] -> 512 x 2MB large pages each
    // Total coverage: 512 PDPT entries x 512 x 2MB = 512 GB
    //
    // Memory required:
    //   1 page  PML4
    //   1 page  PDPT
    //   512 pages PD  (one per PDPT entry)
    //   = 514 pages total (~2 MB)  (safely fits in our 8 MB heap)

    // 1. PML4
    EFI_PHYSICAL_ADDRESS Pml4Addr = MemAllocatePages(1);
    if (!Pml4Addr) { ComPrint("[!] EPT: Failed to allocate PML4.\r\n"); return EptPointer; }
    SetMem((VOID*)Pml4Addr, 4096, 0);

    // 2. PDPT
    EFI_PHYSICAL_ADDRESS PdptAddr = MemAllocatePages(1);
    if (!PdptAddr) { ComPrint("[!] EPT: Failed to allocate PDPT.\r\n"); return EptPointer; }
    SetMem((VOID*)PdptAddr, 4096, 0);

    // 3. 512 PDs  (512 GB / 1 GB per PDPT entry = 512 entries, each needs one 4 KB page)
    #define EPT_PDPT_ENTRIES 512
    EFI_PHYSICAL_ADDRESS PdAddr[EPT_PDPT_ENTRIES];
    for (UINTN i = 0; i < EPT_PDPT_ENTRIES; i++) {
        PdAddr[i] = MemAllocatePages(1);
        if (!PdAddr[i]) {
            ComPrint("[!] EPT: Failed to allocate PD.\r\n");
            return EptPointer;
        }
        SetMem((VOID*)PdAddr[i], 4096, 0);
    }

    // 4. Wire PML4[0] -> PDPT
    PEPT_PML4E Pml4 = (PEPT_PML4E)Pml4Addr;
    Pml4[0].Fields.ReadAccess    = 1;
    Pml4[0].Fields.WriteAccess   = 1;
    Pml4[0].Fields.ExecuteAccess = 1;
    Pml4[0].Fields.PdptAddress   = (PdptAddr / EFI_PAGE_SIZE);

    // 5. Wire PDPT[0..511] -> PDs and fill each PD with 512 x 2MB identity pages
    PEPT_PDPTE Pdpt = (PEPT_PDPTE)PdptAddr;
    for (UINTN i = 0; i < EPT_PDPT_ENTRIES; i++) {
        Pdpt[i].Fields.ReadAccess    = 1;
        Pdpt[i].Fields.WriteAccess   = 1;
        Pdpt[i].Fields.ExecuteAccess = 1;
        Pdpt[i].Fields.PdAddress     = (PdAddr[i] / EFI_PAGE_SIZE);

        PEPT_PDE_2MB Pd = (PEPT_PDE_2MB)PdAddr[i];
        for (UINTN j = 0; j < 512; j++) {
            UINTN PdeIndex = (i * 512) + j; // global 2MB frame number
            Pd[j].Fields.ReadAccess      = 1;
            Pd[j].Fields.WriteAccess     = 1;
            Pd[j].Fields.ExecuteAccess   = 1;
            Pd[j].Fields.MemoryType      = 6; // Write-Back
            Pd[j].Fields.LargePage       = 1;
            Pd[j].Fields.PhysicalAddress = PdeIndex; // GPA == HPA identity
        }
    }

    // 6. Build EPTP
    EptPointer.Fields.MemoryType    = 6; // Write-Back
    EptPointer.Fields.PageWalkLength = 3; // 4-level walk (value = levels - 1)
    EptPointer.Fields.Pml4Address   = (Pml4Addr / EFI_PAGE_SIZE);

    ComPrint("[+] EPT 1:1 Identity Map (512GB) Built Successfully. EPTP: ");
    ComPrintHex(EptPointer.All);
    ComPrint("\r\n");

    g_EptPointer = EptPointer;
    return EptPointer;
}

// Splits a 2MB page in EPT into 512 x 4KB pages
BOOLEAN SafePageSplit2MB(UINT64 PhysicalAddress)
{
    // 1. Walk EPT to get the PDE
    UINT64 PML4Addr = g_EptPointer.Fields.Pml4Address * EFI_PAGE_SIZE;
    if (!PML4Addr) return FALSE;
    
    UINT64 Pml4Index = (PhysicalAddress >> 39) & 0x1FF;
    UINT64 PdptIndex = (PhysicalAddress >> 30) & 0x1FF;
    UINT64 PdIndex   = (PhysicalAddress >> 21) & 0x1FF;
    
    PEPT_PML4E Pml4 = (PEPT_PML4E)PML4Addr;
    if (!Pml4[Pml4Index].Fields.ReadAccess) return FALSE;
    
    UINT64 PdptAddr = Pml4[Pml4Index].Fields.PdptAddress * EFI_PAGE_SIZE;
    PEPT_PDPTE Pdpt = (PEPT_PDPTE)PdptAddr;
    if (!Pdpt[PdptIndex].Fields.ReadAccess) return FALSE;
    
    UINT64 PdAddr = Pdpt[PdptIndex].Fields.PdAddress * EFI_PAGE_SIZE;
    PEPT_PDE_2MB PdeLarge = &((PEPT_PDE_2MB)PdAddr)[PdIndex];
    
    // If it's already split (LargePage == 0), return TRUE (success)
    if (PdeLarge->Fields.LargePage == 0) {
        return TRUE; 
    }
    
    // 2. Allocate 1 page for the Page Table
    EFI_PHYSICAL_ADDRESS PtAddr = MemAllocatePages(1);
    if (!PtAddr) return FALSE;
    
    PEPT_PTE Pt = (PEPT_PTE)PtAddr;
    UINT64 BasePhys = PdeLarge->Fields.PhysicalAddress * 0x200000; // Physical address mapped by the 2MB page
    
    // 3. Fill the new 512 PTEs
    for (UINTN i = 0; i < 512; i++) {
        Pt[i].All = 0;
        Pt[i].Fields.ReadAccess = PdeLarge->Fields.ReadAccess;
        Pt[i].Fields.WriteAccess = PdeLarge->Fields.WriteAccess;
        Pt[i].Fields.ExecuteAccess = PdeLarge->Fields.ExecuteAccess;
        Pt[i].Fields.MemoryType = PdeLarge->Fields.MemoryType;
        Pt[i].Fields.IgnorePAT = PdeLarge->Fields.IgnorePAT;
        Pt[i].Fields.Accessed = PdeLarge->Fields.Accessed;
        Pt[i].Fields.Dirty = PdeLarge->Fields.Dirty;
        Pt[i].Fields.ExecuteForUserMode = PdeLarge->Fields.ExecuteForUserMode;
        Pt[i].Fields.PhysicalAddress = (BasePhys + (i * 0x1000)) / 0x1000;
    }
    
    // 4. Overwrite PDE to point to the new Pt
    PEPT_PDE PdeNormal = (PEPT_PDE)PdeLarge;
    PdeNormal->All = 0;
    PdeNormal->Fields.ReadAccess = 1;
    PdeNormal->Fields.WriteAccess = 1;
    PdeNormal->Fields.ExecuteAccess = 1;
    PdeNormal->Fields.PtAddress = PtAddr / 0x1000;
    
    // 5. Invalidate EPT TLB cache
    struct {
        UINT64 Eptp;
        UINT64 Reserved;
    } Descriptor;
    Descriptor.Eptp = g_EptPointer.All;
    Descriptor.Reserved = 0;
    
    __invept(2, &Descriptor); // 2 = INVEPT_ALL_CONTEXTS
    
    return TRUE;
}

// Deploys a passive VTable hook using EPT Shadowing (Page cloning & swap)
BOOLEAN DeployEpHook(UINT64 GuestCr3, UINT64 TargetVirtualAddress, UINT64 DetourVirtualAddress, UINT64* OutOriginalFunction)
{
    // 1. Translate GVA to GPA
    UINT64 TargetGpa = TranslateGuestVirtual(GuestCr3, TargetVirtualAddress);
    if (!TargetGpa) return FALSE;
    
    UINT64 DetourGpa = TranslateGuestVirtual(GuestCr3, DetourVirtualAddress);
    if (!DetourGpa) return FALSE;
    
    // 2. Ensure target page is split
    if (!SafePageSplit2MB(TargetGpa)) return FALSE;
    
    // Find EPT PTE for target page
    UINT64 PML4Addr = g_EptPointer.Fields.Pml4Address * EFI_PAGE_SIZE;
    UINT64 Pml4Index = (TargetGpa >> 39) & 0x1FF;
    UINT64 PdptIndex = (TargetGpa >> 30) & 0x1FF;
    UINT64 PdIndex   = (TargetGpa >> 21) & 0x1FF;
    UINT64 PtIndex   = (TargetGpa >> 12) & 0x1FF;
    
    PEPT_PML4E Pml4 = (PEPT_PML4E)PML4Addr;
    PEPT_PDPTE Pdpt = (PEPT_PDPTE)(Pml4[Pml4Index].Fields.PdptAddress * EFI_PAGE_SIZE);
    PEPT_PDE Pde = &((PEPT_PDE)(Pdpt[PdptIndex].Fields.PdAddress * EFI_PAGE_SIZE))[PdIndex];
    PEPT_PTE Pt = (PEPT_PTE)(Pde->Fields.PtAddress * EFI_PAGE_SIZE);
    PEPT_PTE TargetPte = &Pt[PtIndex];
    
    // 3. Find/Assign an EPT Hook Entry slot
    INTN FreeSlot = -1;
    for (UINTN i = 0; i < MAX_EPT_HOOKS; i++) {
        if (g_EptHooks[i].IsActive && g_EptHooks[i].OriginalPhysAddress == (TargetGpa & ~0xFFFULL)) {
            // Hook already exists for this page
            return FALSE;
        }
        if (!g_EptHooks[i].IsActive && FreeSlot == -1) {
            FreeSlot = (INTN)i;
        }
    }
    if (FreeSlot == -1) return FALSE; // No slots left
    
    // 4. Allocate Shadow Page (4KB)
    EFI_PHYSICAL_ADDRESS ShadowPage = MemAllocatePages(1);
    if (!ShadowPage) return FALSE;
    
    // Clone original page content into shadow page
    CopyMem((VOID*)ShadowPage, (VOID*)(TargetGpa & ~0xFFFULL), 0x1000);
    
    // 5. Overwrite the virtual function pointer in the shadow page
    UINT64 Offset = TargetGpa & 0xFFF;
    UINT64* TargetPointer = (UINT64*)(ShadowPage + Offset);
    *OutOriginalFunction = *TargetPointer; // Return the original pointer to user
    *TargetPointer = DetourVirtualAddress; // Overwrite shadow page with detour GVA
    
    // 6. Setup hook record
    g_EptHooks[FreeSlot].IsActive = TRUE;
    g_EptHooks[FreeSlot].OriginalPhysAddress = TargetGpa & ~0xFFFULL;
    g_EptHooks[FreeSlot].HookedPhysAddress = ShadowPage;
    g_EptHooks[FreeSlot].VirtualAddress = TargetVirtualAddress;
    g_EptHooks[FreeSlot].OriginalPteValue = TargetPte->All;
    g_EptHooks[FreeSlot].TargetPte = TargetPte;
    
    // 7. Perform the EPT Swap!
    TargetPte->Fields.PhysicalAddress = ShadowPage / 0x1000;
    
    // 8. Flush EPT Cache
    struct {
        UINT64 Eptp;
        UINT64 Reserved;
    } Descriptor;
    Descriptor.Eptp = g_EptPointer.All;
    Descriptor.Reserved = 0;
    __invept(2, &Descriptor);
    
    return TRUE;
}

// Removes a deployed EPT Shadow hook and restores the original EPT page translation
BOOLEAN RemoveEpHook(UINT64 GuestCr3, UINT64 TargetVirtualAddress)
{
    UINT64 TargetGpa = TranslateGuestVirtual(GuestCr3, TargetVirtualAddress);
    if (!TargetGpa) return FALSE;
    
    UINT64 TargetPagePhys = TargetGpa & ~0xFFFULL;
    
    // Find hook entry
    INTN FoundSlot = -1;
    for (UINTN i = 0; i < MAX_EPT_HOOKS; i++) {
        if (g_EptHooks[i].IsActive && g_EptHooks[i].OriginalPhysAddress == TargetPagePhys) {
            FoundSlot = (INTN)i;
            break;
        }
    }
    if (FoundSlot == -1) return FALSE; // Hook not found
    
    // Restore original PTE
    g_EptHooks[FoundSlot].TargetPte->All = g_EptHooks[FoundSlot].OriginalPteValue;
    
    // Flush EPT Cache
    struct {
        UINT64 Eptp;
        UINT64 Reserved;
    } Descriptor;
    Descriptor.Eptp = g_EptPointer.All;
    Descriptor.Reserved = 0;
    __invept(2, &Descriptor);
    
    // Deactivate slot
    g_EptHooks[FoundSlot].IsActive = FALSE;
    g_EptHooks[FoundSlot].OriginalPhysAddress = 0;
    g_EptHooks[FoundSlot].HookedPhysAddress = 0;
    g_EptHooks[FoundSlot].VirtualAddress = 0;
    g_EptHooks[FoundSlot].OriginalPteValue = 0;
    g_EptHooks[FoundSlot].TargetPte = NULL;
    
    return TRUE;
}
