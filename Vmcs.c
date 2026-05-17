#include "Vmcs.h"
#include "VmxHelper.h"
#include "PhysicalMemory.h"
#include "GuestRegisters.h"
#include "VmexitDispatcher.h"
#include "ComLogger.h"
#include "Ept.h"
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <intrin.h>


// VMCS field encodings
#define PIN_BASED_VM_EXEC_CONTROL       0x00004000
#define CPU_BASED_VM_EXEC_CONTROL       0x00004002
#define SECONDARY_VM_EXEC_CONTROL       0x0000401E
#define VM_EXIT_CONTROLS                0x0000400C
#define VM_ENTRY_CONTROLS               0x00004012
#define EXCEPTION_BITMAP                0x00004004
#define MSR_BITMAP                      0x00002004
#define EPT_POINTER                     0x0000201A

// CR0/CR4 Read Shadows and Guest-Host Masks
#define CR0_GUEST_HOST_MASK             0x00006000
#define CR4_GUEST_HOST_MASK             0x00006002
#define CR0_READ_SHADOW                 0x00006004
#define CR4_READ_SHADOW                 0x00006006

// VM-Execution Control Bits
#define CPU_BASED_USE_MSR_BITMAPS       0x10000000

// Guest state
#define GUEST_ES_SELECTOR               0x00000800
#define GUEST_CS_SELECTOR               0x00000802
#define GUEST_SS_SELECTOR               0x00000804
#define GUEST_DS_SELECTOR               0x00000806
#define GUEST_FS_SELECTOR               0x00000808
#define GUEST_GS_SELECTOR               0x0000080A
#define GUEST_LDTR_SELECTOR             0x0000080C
#define GUEST_TR_SELECTOR               0x0000080E
#define GUEST_ES_LIMIT                  0x00004800
#define GUEST_CS_LIMIT                  0x00004802
#define GUEST_SS_LIMIT                  0x00004804
#define GUEST_DS_LIMIT                  0x00004806
#define GUEST_FS_LIMIT                  0x00004808
#define GUEST_GS_LIMIT                  0x0000480A
#define GUEST_LDTR_LIMIT                0x0000480C
#define GUEST_TR_LIMIT                  0x0000480E
#define GUEST_GDTR_LIMIT                0x00004810
#define GUEST_IDTR_LIMIT                0x00004812
#define GUEST_ES_AR                     0x00004814
#define GUEST_CS_AR                     0x00004816
#define GUEST_SS_AR                     0x00004818
#define GUEST_DS_AR                     0x0000481A
#define GUEST_FS_AR                     0x0000481C
#define GUEST_GS_AR                     0x0000481E
#define GUEST_LDTR_AR                   0x00004820
#define GUEST_TR_AR                     0x00004822
#define GUEST_INTERRUPTIBILITY          0x00004824
#define GUEST_ACTIVITY_STATE            0x00004826
#define GUEST_SYSENTER_CS               0x0000482A
#define GUEST_CR0                       0x00006800
#define GUEST_CR3                       0x00006802
#define GUEST_CR4                       0x00006804
#define GUEST_ES_BASE                   0x00006806
#define GUEST_CS_BASE                   0x00006808
#define GUEST_SS_BASE                   0x0000680A
#define GUEST_DS_BASE                   0x0000680C
#define GUEST_FS_BASE                   0x0000680E
#define GUEST_GS_BASE                   0x00006810
#define GUEST_LDTR_BASE                 0x00006812
#define GUEST_TR_BASE                   0x00006814
#define GUEST_GDTR_BASE                 0x00006816
#define GUEST_IDTR_BASE                 0x00006818
#define GUEST_DR7                       0x0000681A
#define GUEST_RSP                       0x0000681C
#define GUEST_RIP                       0x0000681E
#define GUEST_RFLAGS                    0x00006820
#define GUEST_SYSENTER_ESP              0x00006824
#define GUEST_SYSENTER_EIP              0x00006826
#define GUEST_DEBUGCTL                  0x00002802
#define GUEST_VMCS_LINK_PTR             0x00002800
#define GUEST_EFER                      0x00002806

// Host state
#define HOST_ES_SELECTOR                0x00000C00
#define HOST_CS_SELECTOR                0x00000C02
#define HOST_SS_SELECTOR                0x00000C04
#define HOST_DS_SELECTOR                0x00000C06
#define HOST_FS_SELECTOR                0x00000C08
#define HOST_GS_SELECTOR                0x00000C0A
#define HOST_TR_SELECTOR                0x00000C0C
#define HOST_CR0                        0x00006C00
#define HOST_CR3                        0x00006C02
#define HOST_CR4                        0x00006C04
#define HOST_FS_BASE                    0x00006C06
#define HOST_GS_BASE                    0x00006C08
#define HOST_TR_BASE                    0x00006C0A
#define HOST_GDTR_BASE                  0x00006C0C
#define HOST_IDTR_BASE                  0x00006C0E
#define HOST_SYSENTER_ESP               0x00006C10
#define HOST_SYSENTER_EIP               0x00006C12
#define HOST_RSP                        0x00006C14
#define HOST_RIP                        0x00006C16
#define HOST_SYSENTER_CS                0x00004C00
#define HOST_EFER                       0x00002C02

// MSRs
#define MSR_IA32_VMX_TRUE_PINBASED_CTLS     0x48D
#define MSR_IA32_VMX_TRUE_PROCBASED_CTLS    0x48E
#define MSR_IA32_VMX_PROCBASED_CTLS2        0x48B
#define MSR_IA32_VMX_TRUE_EXIT_CTLS         0x48F
#define MSR_IA32_VMX_TRUE_ENTRY_CTLS        0x490
#define MSR_IA32_EFER                       0xC0000080
#define MSR_IA32_FS_BASE                    0xC0000100
#define MSR_IA32_GS_BASE                    0xC0000101
#define MSR_IA32_SYSENTER_CS                0x174
#define MSR_IA32_SYSENTER_ESP               0x175
#define MSR_IA32_SYSENTER_EIP               0x176
#define MSR_IA32_DEBUGCTL                   0x1D9
#define MSR_IA32_RTIT_CTL                  0x570
#define MSR_IA32_LSTAR                     0xC0000082


// CPU-based controls bits
#define CPU_SECONDARY_CONTROLS              (1u << 31)

// Secondary controls bits
#define SECONDARY_ENABLE_RDTSCP             (1u << 3)
#define SECONDARY_ENABLE_INVPCID            (1u << 12)
#define SECONDARY_ENABLE_XSAVE              (1u << 20)

// VM-exit controls bits
#define VMEXIT_HOST_ADDR_SPACE_SIZE         (1u << 9)
#define VMEXIT_LOAD_IA32_EFER               (1u << 21)
#define VMEXIT_SAVE_IA32_EFER               (1u << 20)

// VM-entry controls bits  
#define VMENTRY_IA32E_GUEST                 (1u << 9)
#define VMENTRY_LOAD_IA32_EFER              (1u << 15)

// GDT entry structures for parsing
#pragma pack(push, 1)
typedef struct {
    UINT16 LimitLow;
    UINT16 BaseLow;
    UINT8  BaseMid;
    UINT8  Access;
    UINT8  LimitHighFlags;
    UINT8  BaseHigh;
} GDT_ENTRY, *PGDT_ENTRY;

typedef struct {
    UINT16 LimitLow;
    UINT16 BaseLow;
    UINT8  BaseMid;
    UINT8  Access;
    UINT8  LimitHighFlags;
    UINT8  BaseHigh;
    UINT32 BaseUpper;
    UINT32 Reserved;
} GDT_ENTRY64, *PGDT_ENTRY64;
#pragma pack(pop)

// Adjust a VMX control value using allowed-0/allowed-1 from MSR
static UINT32 AdjustVmxControl(UINT32 Desired, UINT32 MsrAddr)
{
    UINT64 Msr = AsmReadMsr64(MsrAddr);
    UINT32 Mandatory1s = (UINT32)(Msr & 0xFFFFFFFF);  // must be 1
    UINT32 Allowed1s   = (UINT32)(Msr >> 32);          // may be 1
    return (Desired | Mandatory1s) & Allowed1s;
}

static UINT64 GetSegmentBase(UINT64 GdtBase, UINT16 Selector)
{
    if ((Selector & ~0x7) == 0) return 0;
    PGDT_ENTRY E = (PGDT_ENTRY)(GdtBase + (Selector & ~0x7));
    UINT64 Base = ((UINT64)E->BaseLow) | ((UINT64)E->BaseMid << 16) | ((UINT64)E->BaseHigh << 24);
    // System descriptor: S-bit clear
    if ((E->Access & 0x10) == 0) {
        PGDT_ENTRY64 E64 = (PGDT_ENTRY64)E;
        Base |= ((UINT64)E64->BaseUpper << 32);
    }
    return Base;
}

static UINT32 GetSegmentLimit(UINT64 GdtBase, UINT16 Selector)
{
    if ((Selector & ~0x7) == 0) return 0;
    PGDT_ENTRY E = (PGDT_ENTRY)(GdtBase + (Selector & ~0x7));
    UINT32 Limit = ((UINT32)(E->LimitHighFlags & 0x0F) << 16) | E->LimitLow;
    if (E->LimitHighFlags & 0x80) Limit = (Limit << 12) | 0xFFF;
    return Limit;
}

static UINT32 GetSegmentAr(UINT64 GdtBase, UINT16 Selector)
{
    if ((Selector & ~0x7) == 0) return 0x10000; // Unusable
    PGDT_ENTRY E = (PGDT_ENTRY)(GdtBase + (Selector & ~0x7));
    
    // VMCS Access Rights format:
    // Bits 7:0 - Type, S, DPL, P (matches E->Access exactly)
    // Bits 11:8 - Reserved, must be 0
    // Bit 12 - AVL
    // Bit 13 - L (64-bit mode)
    // Bit 14 - D/B (Default operation size)
    // Bit 15 - G (Granularity)
    // Bit 16 - Unusable (0 = usable)
    
    // In GDT_ENTRY, LimitHighFlags (bits 4-7) are AVL, L, D/B, G.
    // So we shift them by 8 to place them in bits 12-15 of the AR.
    UINT32 Ar = (UINT32)(E->Access) | (((UINT32)(E->LimitHighFlags) & 0xF0) << 8);
    return Ar;
}



// Extern declaration for assembly VM-exit entry
extern VOID AsmVmexitHandler(VOID);

// ============================================================
// BuildHostPageTable
// Builds a minimal 4-level identity page table in our
// EfiRuntimeServicesData heap that covers 0..4GB using 2MB
// large pages.  Used as HOST_CR3 so the host VM-exit handler
// always has valid, Windows-preserved page tables.
// Memory cost: 1 PML4 + 1 PDPT + 4 PD = 6 pages (24 KB).
// ============================================================
static UINT64 BuildHostPageTable(VOID)
{
    // PML4
    EFI_PHYSICAL_ADDRESS Pml4 = MemAllocatePages(1);
    if (!Pml4) return 0;
    SetMem((VOID*)Pml4, 4096, 0);

    // PDPT
    EFI_PHYSICAL_ADDRESS Pdpt = MemAllocatePages(1);
    if (!Pdpt) return 0;
    SetMem((VOID*)Pdpt, 4096, 0);

    // 4 PDs  (one per 1GB PDPT slot, each covers 512 x 2MB = 1GB)
    EFI_PHYSICAL_ADDRESS Pd[4];
    for (UINTN i = 0; i < 4; i++) {
        Pd[i] = MemAllocatePages(1);
        if (!Pd[i]) return 0;
        SetMem((VOID*)Pd[i], 4096, 0);
    }

    // PML4[0] -> PDPT  (P, R/W)
    *((UINT64*)Pml4) = Pdpt | 0x3;

    // PDPT[0..3] -> PDs  (P, R/W)
    UINT64* PdptE = (UINT64*)Pdpt;
    for (UINTN i = 0; i < 4; i++) {
        PdptE[i] = Pd[i] | 0x3;
    }

    // Each PD: 512 x 2MB large pages (P, R/W, PS)
    for (UINTN i = 0; i < 4; i++) {
        UINT64* PdE = (UINT64*)Pd[i];
        for (UINTN j = 0; j < 512; j++) {
            UINT64 PhysFrame = (UINT64)(i * 512 + j);   // 2MB frame index
            PdE[j] = (PhysFrame << 21) | 0x83;          // P + R/W + PS (2MB)
        }
    }

    return (UINT64)Pml4;  // physical address — used directly as CR3
}

// Helper to find TSS selector in GDT if TR is 0
static UINT16 FindTssSelector(UINT64 GdtBase, UINT16 GdtLimit)
{
    for (UINT16 i = 8; i < GdtLimit; i += 8) {
        PGDT_ENTRY E = (PGDT_ENTRY)(GdtBase + i);
        // S-bit (bit 4) must be 0 for system descriptors
        if ((E->Access & 0x10) == 0) {
            UINT8 Type = E->Access & 0x0F;
            if (Type == 9 || Type == 11) return i; // 64-bit TSS
        }
        // If it's a 64-bit system descriptor, it takes 16 bytes
        if ((E->Access & 0x10) == 0) i += 8;
    }
    return 0;
}

BOOLEAN InitializeVmcs(VOID)
{
    // 1. Allocate and init VMCS region
    EFI_PHYSICAL_ADDRESS VmcsPhys = MemAllocatePages(1);
    if (!VmcsPhys) { ComPrint("[!] VMCS alloc failed.\r\n"); return FALSE; }
    SetMem((VOID*)VmcsPhys, 4096, 0);
    
    PVMCS_REGION Vmcs = (PVMCS_REGION)VmcsPhys;
    UINT64 VmxBasic = AsmReadMsr64(MSR_IA32_VMX_BASIC);
    Vmcs->RevisionId = (UINT32)(VmxBasic & 0x7FFFFFFF);

    if (__vmx_vmclear((unsigned long long*)&VmcsPhys) != 0) { ComPrint("[!] VMCLEAR failed.\r\n"); return FALSE; }
    if (__vmx_vmptrld((unsigned long long*)&VmcsPhys) != 0) { ComPrint("[!] VMPTRLD failed.\r\n"); return FALSE; }

    UINTN Cr0 = AsmReadCr0();
    UINTN Cr3 = AsmReadCr3();
    UINTN Cr4 = AsmReadCr4();

    IA32_DESCRIPTOR Gdtr, Idtr;
    AsmReadGdtr(&Gdtr);
    AsmReadIdtr(&Idtr);
    UINT16 CsSel = AsmReadCs();
    UINT16 SsSel = AsmReadSs();
    UINT64 GdtBase = Gdtr.Base;

    // --- VMX TR Synthesis & GDT Relocation (HyperVenom style) ---
    // 1. Relocate and expand GDT in heap to host our 16-byte TSS descriptor safely.
    EFI_PHYSICAL_ADDRESS GdtDest = MemAllocatePages(1);
    if (!GdtDest) { ComPrint("[!] GDT alloc failed.\r\n"); return FALSE; }
    SetMem((VOID*)GdtDest, 4096, 0);
    CopyMem((VOID*)GdtDest, (VOID*)Gdtr.Base, Gdtr.Limit + 1);

    // 2. Allocate a dedicated physical page for TSS to guarantee correctness in VMX root/non-root.
    EFI_PHYSICAL_ADDRESS TssPhys = MemAllocatePages(1);
    if (!TssPhys) { ComPrint("[!] TSS alloc failed.\r\n"); return FALSE; }
    SetMem((VOID*)TssPhys, 4096, 0);

    // Setup expanded TSS descriptor at selector 0x40 in relocated GDT (GDT_ENTRY64 format)
    PGDT_ENTRY64 TssDesc = (PGDT_ENTRY64)((UINT8*)GdtDest + 0x40);
    TssDesc->LimitLow = 0x67;
    TssDesc->BaseLow = (UINT16)(TssPhys & 0xFFFF);
    TssDesc->BaseMid = (UINT8)((TssPhys >> 16) & 0xFF);
    TssDesc->Access = 0x8B; // Present, Busy 64-bit TSS (Type 11)
    TssDesc->LimitHighFlags = 0x00;
    TssDesc->BaseHigh = (UINT8)((TssPhys >> 24) & 0xFF);
    TssDesc->BaseUpper = (UINT32)(TssPhys >> 32);
    TssDesc->Reserved = 0;

    UINT16 TrSel = 0x40; // Dedicated TSS selector in our custom GDT
    UINT32 GuestTrLimit = 0x67;
    UINT32 GuestTrAr = 0x008B; // Present, DPL 0, Busy 64-bit TSS
    UINT64 GuestTrBase = TssPhys;

    // 3. Set VM-Execution Controls
    UINT32 PinCtls = AdjustVmxControl(0, MSR_IA32_VMX_TRUE_PINBASED_CTLS);
    UINT32 ProcCtls = AdjustVmxControl(CPU_SECONDARY_CONTROLS | CPU_BASED_USE_MSR_BITMAPS, MSR_IA32_VMX_TRUE_PROCBASED_CTLS);
    
    // Enable EPT (bit 1) in Secondary Controls
    UINT32 Proc2Ctls = AdjustVmxControl(0x00000002, MSR_IA32_VMX_PROCBASED_CTLS2);
    
    UINT32 ExitCtls = AdjustVmxControl(VMEXIT_HOST_ADDR_SPACE_SIZE | VMEXIT_SAVE_IA32_EFER | VMEXIT_LOAD_IA32_EFER, MSR_IA32_VMX_TRUE_EXIT_CTLS);
    UINT32 EntryCtls = AdjustVmxControl(VMENTRY_IA32E_GUEST | VMENTRY_LOAD_IA32_EFER, MSR_IA32_VMX_TRUE_ENTRY_CTLS);

    // Write EPTP to VMCS
    extern EPTP g_EptPointer;
    __vmx_vmwrite(EPT_POINTER, g_EptPointer.All);

    // Allocate 4KB MSR Bitmap (filled with 0 = no MSRs cause VM-Exits)
    EFI_PHYSICAL_ADDRESS MsrBitmap = MemAllocatePages(1);
    if (!MsrBitmap) { ComPrint("[!] MSR Bitmap alloc failed.\r\n"); return FALSE; }
    SetMem((VOID*)MsrBitmap, 4096, 0);
    __vmx_vmwrite(MSR_BITMAP, MsrBitmap);

    // Intercept IA32_FEATURE_CONTROL (0x3A) to hide VMX BIOS enablement.
    // If Windows sees VMX is enabled in BIOS, it will try to launch Hyper-V/VBS,
    // execute VMXON/VMLAUNCH, which we silently skip, causing a boot freeze.
    VmxSetMsrBitmap((VOID*)MsrBitmap, 0x3A, TRUE, FALSE);

    __vmx_vmwrite(PIN_BASED_VM_EXEC_CONTROL, PinCtls);
    __vmx_vmwrite(CPU_BASED_VM_EXEC_CONTROL, ProcCtls);
    __vmx_vmwrite(SECONDARY_VM_EXEC_CONTROL, Proc2Ctls);
    __vmx_vmwrite(VM_EXIT_CONTROLS, ExitCtls);
    __vmx_vmwrite(VM_ENTRY_CONTROLS, EntryCtls);
    __vmx_vmwrite(EXCEPTION_BITMAP, 0);

    // CR0/CR4 Shadows and Masks to prevent #GP on guest writes
    __vmx_vmwrite(CR0_GUEST_HOST_MASK, 0); // Allow guest full control of CR0
    __vmx_vmwrite(CR4_GUEST_HOST_MASK, 0x2000); // Mask VMXE (bit 13) to prevent guest GP on CR4 writes
    __vmx_vmwrite(CR0_READ_SHADOW, Cr0);
    __vmx_vmwrite(CR4_READ_SHADOW, Cr4 & ~0x2000); // Hide VMXE from guest

    // 4. Guest State
    UINT16 EsSel = AsmReadEs();
    UINT16 DsSel = AsmReadDs();
    UINT16 FsSel = AsmReadFs();
    UINT16 GsSel = AsmReadGs();

    __vmx_vmwrite(GUEST_CS_SELECTOR, CsSel);
    __vmx_vmwrite(GUEST_SS_SELECTOR, SsSel);
    __vmx_vmwrite(GUEST_DS_SELECTOR, DsSel);
    __vmx_vmwrite(GUEST_ES_SELECTOR, EsSel);
    __vmx_vmwrite(GUEST_FS_SELECTOR, FsSel);
    __vmx_vmwrite(GUEST_GS_SELECTOR, GsSel);
    __vmx_vmwrite(GUEST_TR_SELECTOR, TrSel); 
    __vmx_vmwrite(GUEST_LDTR_SELECTOR, 0);

    __vmx_vmwrite(GUEST_CS_LIMIT,   GetSegmentLimit(GdtBase, CsSel));
    __vmx_vmwrite(GUEST_SS_LIMIT,   GetSegmentLimit(GdtBase, SsSel));
    __vmx_vmwrite(GUEST_DS_LIMIT,   GetSegmentLimit(GdtBase, DsSel));
    __vmx_vmwrite(GUEST_ES_LIMIT,   GetSegmentLimit(GdtBase, EsSel));
    __vmx_vmwrite(GUEST_FS_LIMIT,   GetSegmentLimit(GdtBase, FsSel));
    __vmx_vmwrite(GUEST_GS_LIMIT,   GetSegmentLimit(GdtBase, GsSel));
    __vmx_vmwrite(GUEST_TR_LIMIT,   GuestTrLimit);
    __vmx_vmwrite(GUEST_LDTR_LIMIT, 0xFFFF);
    UINT16 NewGdtLimit = Gdtr.Limit;
    if (NewGdtLimit < 0x4F) {
        NewGdtLimit = 0x4F;
    }
    __vmx_vmwrite(GUEST_GDTR_LIMIT, NewGdtLimit);
    __vmx_vmwrite(GUEST_IDTR_LIMIT, Idtr.Limit);

    __vmx_vmwrite(GUEST_CS_AR,   GetSegmentAr(GdtBase, CsSel));
    __vmx_vmwrite(GUEST_SS_AR,   GetSegmentAr(GdtBase, SsSel));
    __vmx_vmwrite(GUEST_DS_AR,   GetSegmentAr(GdtBase, DsSel));
    __vmx_vmwrite(GUEST_ES_AR,   GetSegmentAr(GdtBase, EsSel));
    __vmx_vmwrite(GUEST_FS_AR,   GetSegmentAr(GdtBase, FsSel));
    __vmx_vmwrite(GUEST_GS_AR,   GetSegmentAr(GdtBase, GsSel));
    __vmx_vmwrite(GUEST_TR_AR,   GuestTrAr);
    __vmx_vmwrite(GUEST_LDTR_AR, 0x10000); 

    __vmx_vmwrite(GUEST_CS_BASE,   GetSegmentBase(GdtBase, CsSel));
    __vmx_vmwrite(GUEST_SS_BASE,   GetSegmentBase(GdtBase, SsSel));
    __vmx_vmwrite(GUEST_DS_BASE,   GetSegmentBase(GdtBase, DsSel));
    __vmx_vmwrite(GUEST_ES_BASE,   GetSegmentBase(GdtBase, EsSel));
    __vmx_vmwrite(GUEST_FS_BASE,   AsmReadMsr64(MSR_IA32_FS_BASE));
    __vmx_vmwrite(GUEST_GS_BASE,   AsmReadMsr64(MSR_IA32_GS_BASE));
    __vmx_vmwrite(GUEST_TR_BASE,   GuestTrBase);
    __vmx_vmwrite(GUEST_LDTR_BASE, 0);
    __vmx_vmwrite(GUEST_GDTR_BASE, GdtDest); // Use relocated GDT that has TSS descriptor
    __vmx_vmwrite(GUEST_IDTR_BASE, Idtr.Base);

    __vmx_vmwrite(GUEST_CR0,   Cr0);
    __vmx_vmwrite(GUEST_CR3,   Cr3);
    __vmx_vmwrite(GUEST_CR4,   Cr4);
    __vmx_vmwrite(GUEST_DR7,   0x400);
    __vmx_vmwrite(GUEST_RFLAGS, AsmReadEflags()); // Use exact system flags

    __vmx_vmwrite(GUEST_SYSENTER_CS,  AsmReadMsr64(MSR_IA32_SYSENTER_CS));
    __vmx_vmwrite(GUEST_SYSENTER_ESP, AsmReadMsr64(MSR_IA32_SYSENTER_ESP));
    __vmx_vmwrite(GUEST_SYSENTER_EIP, AsmReadMsr64(MSR_IA32_SYSENTER_EIP));
    __vmx_vmwrite(GUEST_DEBUGCTL,     AsmReadMsr64(MSR_IA32_DEBUGCTL));
    __vmx_vmwrite(GUEST_EFER,         AsmReadMsr64(MSR_IA32_EFER));
    __vmx_vmwrite(GUEST_VMCS_LINK_PTR, 0xFFFFFFFFFFFFFFFF); 

    __vmx_vmwrite(GUEST_ACTIVITY_STATE, 0);     
    __vmx_vmwrite(GUEST_INTERRUPTIBILITY, 0);   
    __vmx_vmwrite(GUEST_RSP, 0); 
    __vmx_vmwrite(GUEST_RIP, 0); 

    // 5. Host State (Intel Error 0x8 points here)

    // Intel Rule: Host selectors must be non-zero and RPL=0
    // In UEFI, DS/ES/FS/GS are often 0. We use SS instead.
    __vmx_vmwrite(HOST_CS_SELECTOR, CsSel & ~0x7);
    __vmx_vmwrite(HOST_SS_SELECTOR, SsSel & ~0x7);
    __vmx_vmwrite(HOST_DS_SELECTOR, SsSel & ~0x7); // Use SS
    __vmx_vmwrite(HOST_ES_SELECTOR, SsSel & ~0x7); // Use SS
    __vmx_vmwrite(HOST_FS_SELECTOR, SsSel & ~0x7); // Use SS
    __vmx_vmwrite(HOST_GS_SELECTOR, SsSel & ~0x7); // Use SS
    
    __vmx_vmwrite(HOST_TR_SELECTOR, TrSel & ~0x7);

    __vmx_vmwrite(HOST_CR0, Cr0);
    __vmx_vmwrite(HOST_CR3, Cr3);
    __vmx_vmwrite(HOST_CR4, Cr4);

    __vmx_vmwrite(HOST_FS_BASE,   AsmReadMsr64(MSR_IA32_FS_BASE));
    __vmx_vmwrite(HOST_GS_BASE,   AsmReadMsr64(MSR_IA32_GS_BASE));
    __vmx_vmwrite(HOST_TR_BASE,   GuestTrBase);
    __vmx_vmwrite(HOST_GDTR_BASE, GdtDest);
    __vmx_vmwrite(HOST_IDTR_BASE, Idtr.Base);

    __vmx_vmwrite(HOST_SYSENTER_CS,  AsmReadMsr64(MSR_IA32_SYSENTER_CS));
    __vmx_vmwrite(HOST_SYSENTER_ESP, AsmReadMsr64(MSR_IA32_SYSENTER_ESP));
    __vmx_vmwrite(HOST_SYSENTER_EIP, AsmReadMsr64(MSR_IA32_SYSENTER_EIP));
    __vmx_vmwrite(HOST_EFER, AsmReadMsr64(MSR_IA32_EFER));

    // ==================================================================
    // Relocate all host-mode structures to EfiRuntimeServicesData heap
    // ==================================================================

    // 1. Copy VM-Exit handler to heap.
    //    The handler page has a 16-byte debug header before the code:
    //      [HandlerDest + 0]  UINT64  debug-log page physical address
    //      [HandlerDest + 8]  UINT64  VM-exit counter (starts at 0)
    //      [HandlerDest +16]  handler machine code  <- HOST_RIP points here
    UINTN HandlerSize = (UINTN)MinimalVmexitHandlerEnd - (UINTN)MinimalVmexitHandlerStart;
    EFI_PHYSICAL_ADDRESS HandlerDest = MemAllocatePages(1);
    if (!HandlerDest) { ComPrint("[!] Handler alloc failed.\r\n"); return FALSE; }

    // Separate 4KB page for the exit-reason ring buffer (survives as EfiRuntimeServicesData)
    EFI_PHYSICAL_ADDRESS DebugLogPage = MemAllocatePages(1);
    if (!DebugLogPage) { ComPrint("[!] Debug log page alloc failed.\r\n"); return FALSE; }

    *((UINT64*)(HandlerDest + 0)) = (UINT64)DebugLogPage;
    *((UINT64*)(HandlerDest + 8)) = 0ULL;
    CopyMem((VOID*)(HandlerDest + 16), (VOID*)MinimalVmexitHandlerStart, HandlerSize);

    ComPrint("[*] Handler base: 0x"); ComPrintHex((UINT64)HandlerDest);
    ComPrint("  DebugLog: 0x");      ComPrintHex((UINT64)DebugLogPage);
    ComPrint("\r\n");

    // 3. Copy IDT to heap
    EFI_PHYSICAL_ADDRESS IdtDest = MemAllocatePages(1);
    if (!IdtDest) { ComPrint("[!] IDT alloc failed.\r\n"); return FALSE; }
    CopyMem((VOID*)IdtDest, (VOID*)Idtr.Base, Idtr.Limit + 1);

    // 4. Allocate Host Stack in heap (32KB = 8 pages)
    EFI_PHYSICAL_ADDRESS HostStackDest = MemAllocatePages(8);
    if (!HostStackDest) { ComPrint("[!] Host Stack alloc failed.\r\n"); return FALSE; }
    UINT64 HostStackTop = HostStackDest + (8 * 4096);

    // 5. Build host-mode page tables in EfiRuntimeServicesData (never freed by Windows).
    //    UEFI firmware page tables are in EfiBootServicesData which Windows CAN free.
    //    Using them as HOST_CR3 causes triple-fault on the first VM-exit after handoff.
    UINT64 HostCr3 = BuildHostPageTable();
    if (!HostCr3) { ComPrint("[!] Host page table alloc failed.\r\n"); return FALSE; }

    // HOST_RIP = HandlerDest + 16 (skip the 16-byte debug header)
    __vmx_vmwrite(HOST_RSP, HostStackTop);
    __vmx_vmwrite(HOST_RIP, HandlerDest + 16);
    __vmx_vmwrite(HOST_GDTR_BASE, GdtDest);
    __vmx_vmwrite(HOST_IDTR_BASE, IdtDest);
    __vmx_vmwrite(HOST_CR3, HostCr3);

    ComPrint("[+] VMCS fully configured. Ready for VMLAUNCH.\r\n");
    return TRUE;
}
