#ifndef EPT_H
#define EPT_H

#include <Uefi.h>

#pragma pack(push, 1)

// EPT Pointer (EPTP)
typedef union _EPTP {
    UINT64 All;
    struct {
        UINT64 MemoryType : 3;
        UINT64 PageWalkLength : 3;
        UINT64 EnableAccessAndDirtyFlags : 1;
        UINT64 Reserved1 : 5;
        UINT64 Pml4Address : 36;
        UINT64 Reserved2 : 16;
    } Fields;
} EPTP, *PEPTP;

// EPT PML4 Entry
typedef union _EPT_PML4E {
    UINT64 All;
    struct {
        UINT64 ReadAccess : 1;
        UINT64 WriteAccess : 1;
        UINT64 ExecuteAccess : 1;
        UINT64 Reserved1 : 5;
        UINT64 Accessed : 1;
        UINT64 Ignored1 : 1;
        UINT64 ExecuteForUserMode : 1;
        UINT64 Ignored2 : 1;
        UINT64 PdptAddress : 36;
        UINT64 Reserved2 : 16;
    } Fields;
} EPT_PML4E, *PEPT_PML4E;

// EPT PDPT Entry
typedef union _EPT_PDPTE {
    UINT64 All;
    struct {
        UINT64 ReadAccess : 1;
        UINT64 WriteAccess : 1;
        UINT64 ExecuteAccess : 1;
        UINT64 Reserved1 : 5;
        UINT64 Accessed : 1;
        UINT64 Ignored1 : 1;
        UINT64 ExecuteForUserMode : 1;
        UINT64 Ignored2 : 1;
        UINT64 PdAddress : 36;
        UINT64 Reserved2 : 16;
    } Fields;
} EPT_PDPTE, *PEPT_PDPTE;

// EPT PDE (2MB Large Page)
typedef union _EPT_PDE_2MB {
    UINT64 All;
    struct {
        UINT64 ReadAccess : 1;
        UINT64 WriteAccess : 1;
        UINT64 ExecuteAccess : 1;
        UINT64 MemoryType : 3;
        UINT64 IgnorePAT : 1;
        UINT64 LargePage : 1; // Must be 1
        UINT64 Accessed : 1;
        UINT64 Dirty : 1;
        UINT64 ExecuteForUserMode : 1;
        UINT64 Ignored1 : 1;
        UINT64 Reserved1 : 9;
        UINT64 PhysicalAddress : 27;
        UINT64 Reserved2 : 15;
        UINT64 SuppressVE : 1;
    } Fields;
} EPT_PDE_2MB, *PEPT_PDE_2MB;

// EPT PDE (Points to Page Table)
typedef union _EPT_PDE {
    UINT64 All;
    struct {
        UINT64 ReadAccess : 1;
        UINT64 WriteAccess : 1;
        UINT64 ExecuteAccess : 1;
        UINT64 Reserved1 : 5;
        UINT64 Accessed : 1;
        UINT64 Ignored1 : 1;
        UINT64 ExecuteForUserMode : 1;
        UINT64 Ignored2 : 1;
        UINT64 PtAddress : 36;
        UINT64 Reserved2 : 16;
    } Fields;
} EPT_PDE, *PEPT_PDE;

// EPT PTE (4KB Page)
typedef union _EPT_PTE {
    UINT64 All;
    struct {
        UINT64 ReadAccess : 1;
        UINT64 WriteAccess : 1;
        UINT64 ExecuteAccess : 1;
        UINT64 MemoryType : 3;
        UINT64 IgnorePAT : 1;
        UINT64 Ignored1 : 1;
        UINT64 Accessed : 1;
        UINT64 Dirty : 1;
        UINT64 ExecuteForUserMode : 1;
        UINT64 Ignored2 : 1;
        UINT64 Reserved1 : 9;
        UINT64 PhysicalAddress : 27;
        UINT64 Reserved2 : 15;
        UINT64 SuppressVE : 1;
    } Fields;
} EPT_PTE, *PEPT_PTE;

#define MAX_EPT_HOOKS 32

typedef struct _EPTHOOK_ENTRY {
    BOOLEAN IsActive;
    UINT64 OriginalPhysAddress; // GPA of page
    UINT64 HookedPhysAddress;   // GPA of Shadow Page
    UINT64 VirtualAddress;      // GVA
    UINT64 OriginalPteValue;
    PEPT_PTE TargetPte;
} EPTHOOK_ENTRY, *PEPTHOOK_ENTRY;

#pragma pack(pop)

// EPT Initialization
EPTP InitializeEpt(VOID);

// MSVC Compiler Intrinsic for INVEPT
#if defined(_MSC_VER)
void __invept(unsigned long Type, void* Descriptor);
#endif

// HyperVenom EPT Shadowing Engine Prototypes
BOOLEAN SafePageSplit2MB(UINT64 PhysicalAddress);
BOOLEAN DeployEpHook(UINT64 GuestCr3, UINT64 TargetVirtualAddress, UINT64 DetourVirtualAddress, UINT64* OutOriginalFunction);
BOOLEAN RemoveEpHook(UINT64 GuestCr3, UINT64 TargetVirtualAddress);


// Global EPTP storage
extern EPTP g_EptPointer;

#endif // EPT_H
