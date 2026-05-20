#include "Vmcs.h"
#include "ComLogger.h"
#include "Ept.h"
#include "GuestRegisters.h"
#include "PhysicalMemory.h"
#include "VmexitDispatcher.h"
#include "VmxHelper.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <intrin.h>
#include <Protocol/GraphicsOutput.h>

// VMCS field encodings
#define PIN_BASED_VM_EXEC_CONTROL 0x00004000
#define CPU_BASED_VM_EXEC_CONTROL 0x00004002
#define SECONDARY_VM_EXEC_CONTROL 0x0000401E
#define VM_EXIT_CONTROLS 0x0000400C
#define VM_ENTRY_CONTROLS 0x00004012
#define EXCEPTION_BITMAP 0x00004004
#define MSR_BITMAP 0x00002004
#define EPT_POINTER 0x0000201A

// CR0/CR4 Read Shadows and Guest-Host Masks
#define CR0_GUEST_HOST_MASK 0x00006000
#define CR4_GUEST_HOST_MASK 0x00006002
#define CR0_READ_SHADOW 0x00006004
#define CR4_READ_SHADOW 0x00006006

// VM-Execution Control Bits
#define CPU_BASED_USE_MSR_BITMAPS 0x10000000

// Guest state
#define GUEST_ES_SELECTOR 0x00000800
#define GUEST_CS_SELECTOR 0x00000802
#define GUEST_SS_SELECTOR 0x00000804
#define GUEST_DS_SELECTOR 0x00000806
#define GUEST_FS_SELECTOR 0x00000808
#define GUEST_GS_SELECTOR 0x0000080A
#define GUEST_LDTR_SELECTOR 0x0000080C
#define GUEST_TR_SELECTOR 0x0000080E
#define GUEST_ES_LIMIT 0x00004800
#define GUEST_CS_LIMIT 0x00004802
#define GUEST_SS_LIMIT 0x00004804
#define GUEST_DS_LIMIT 0x00004806
#define GUEST_FS_LIMIT 0x00004808
#define GUEST_GS_LIMIT 0x0000480A
#define GUEST_LDTR_LIMIT 0x0000480C
#define GUEST_TR_LIMIT 0x0000480E
#define GUEST_GDTR_LIMIT 0x00004810
#define GUEST_IDTR_LIMIT 0x00004812
#define GUEST_ES_AR 0x00004814
#define GUEST_CS_AR 0x00004816
#define GUEST_SS_AR 0x00004818
#define GUEST_DS_AR 0x0000481A
#define GUEST_FS_AR 0x0000481C
#define GUEST_GS_AR 0x0000481E
#define GUEST_LDTR_AR 0x00004820
#define GUEST_TR_AR 0x00004822
#define GUEST_INTERRUPTIBILITY 0x00004824
#define GUEST_ACTIVITY_STATE 0x00004826
#define GUEST_SYSENTER_CS 0x0000482A
#define GUEST_CR0 0x00006800
#define GUEST_CR3 0x00006802
#define GUEST_CR4 0x00006804
#define GUEST_ES_BASE 0x00006806
#define GUEST_CS_BASE 0x00006808
#define GUEST_SS_BASE 0x0000680A
#define GUEST_DS_BASE 0x0000680C
#define GUEST_FS_BASE 0x0000680E
#define GUEST_GS_BASE 0x00006810
#define GUEST_LDTR_BASE 0x00006812
#define GUEST_TR_BASE 0x00006814
#define GUEST_GDTR_BASE 0x00006816
#define GUEST_IDTR_BASE 0x00006818
#define GUEST_DR7 0x0000681A
#define GUEST_RSP 0x0000681C
#define GUEST_RIP 0x0000681E
#define GUEST_RFLAGS 0x00006820
#define GUEST_SYSENTER_ESP 0x00006824
#define GUEST_SYSENTER_EIP 0x00006826
#define GUEST_DEBUGCTL 0x00002802
#define GUEST_VMCS_LINK_PTR 0x00002800
#define GUEST_EFER 0x00002806

// Host state
#define HOST_ES_SELECTOR 0x00000C00
#define HOST_CS_SELECTOR 0x00000C02
#define HOST_SS_SELECTOR 0x00000C04
#define HOST_DS_SELECTOR 0x00000C06
#define HOST_FS_SELECTOR 0x00000C08
#define HOST_GS_SELECTOR 0x00000C0A
#define HOST_TR_SELECTOR 0x00000C0C
#define HOST_CR0 0x00006C00
#define HOST_CR3 0x00006C02
#define HOST_CR4 0x00006C04
#define HOST_FS_BASE 0x00006C06
#define HOST_GS_BASE 0x00006C08
#define HOST_TR_BASE 0x00006C0A
#define HOST_GDTR_BASE 0x00006C0C
#define HOST_IDTR_BASE 0x00006C0E
#define HOST_SYSENTER_ESP 0x00006C10
#define HOST_SYSENTER_EIP 0x00006C12
#define HOST_RSP 0x00006C14
#define HOST_RIP 0x00006C16
#define HOST_SYSENTER_CS 0x00004C00
#define HOST_EFER 0x00002C02

// MSRs
#define MSR_IA32_VMX_TRUE_PINBASED_CTLS 0x48D
#define MSR_IA32_VMX_TRUE_PROCBASED_CTLS 0x48E
#define MSR_IA32_VMX_PROCBASED_CTLS2 0x48B
#define MSR_IA32_VMX_TRUE_EXIT_CTLS 0x48F
#define MSR_IA32_VMX_TRUE_ENTRY_CTLS 0x490
#define MSR_IA32_EFER 0xC0000080
#define MSR_IA32_FS_BASE 0xC0000100
#define MSR_IA32_GS_BASE 0xC0000101
#define MSR_IA32_SYSENTER_CS 0x174
#define MSR_IA32_SYSENTER_ESP 0x175
#define MSR_IA32_SYSENTER_EIP 0x176
#define MSR_IA32_DEBUGCTL 0x1D9
#define MSR_IA32_RTIT_CTL 0x570
#define MSR_IA32_LSTAR 0xC0000082

// CPU-based controls bits
#define CPU_SECONDARY_CONTROLS (1u << 31)

// Secondary controls bits
#define SECONDARY_ENABLE_RDTSCP (1u << 3)
#define SECONDARY_ENABLE_INVPCID (1u << 12)
#define SECONDARY_ENABLE_XSAVE (1u << 20)

// VM-exit controls bits
#define VMEXIT_HOST_ADDR_SPACE_SIZE (1u << 9)
#define VMEXIT_LOAD_IA32_EFER (1u << 21)
#define VMEXIT_SAVE_IA32_EFER (1u << 20)

// VM-entry controls bits
#define VMENTRY_IA32E_GUEST (1u << 9)
#define VMENTRY_LOAD_IA32_EFER (1u << 15)

// GDT entry structures for parsing
#pragma pack(push, 1)
typedef struct {
  UINT16 LimitLow;
  UINT16 BaseLow;
  UINT8 BaseMid;
  UINT8 Access;
  UINT8 LimitHighFlags;
  UINT8 BaseHigh;
} GDT_ENTRY, *PGDT_ENTRY;

typedef struct {
  UINT16 LimitLow;
  UINT16 BaseLow;
  UINT8 BaseMid;
  UINT8 Access;
  UINT8 LimitHighFlags;
  UINT8 BaseHigh;
  UINT32 BaseUpper;
  UINT32 Reserved;
} GDT_ENTRY64, *PGDT_ENTRY64;
#pragma pack(pop)

// Adjust a VMX control value using allowed-0/allowed-1 from MSR
static UINT32 AdjustVmxControl(UINT32 Desired, UINT32 MsrAddr) {
  UINT64 Basic = AsmReadMsr64(0x480); // MSR_IA32_VMX_BASIC
  UINT32 ActualMsr = MsrAddr;
  
  // If bit 55 of IA32_VMX_BASIC is 0, we must fall back to standard VMX controls MSRs
  if ((Basic & (1ULL << 55)) == 0) {
    switch (MsrAddr) {
      case MSR_IA32_VMX_TRUE_PINBASED_CTLS:  ActualMsr = 0x481; break; // standard PINBASED
      case MSR_IA32_VMX_TRUE_PROCBASED_CTLS: ActualMsr = 0x482; break; // standard PROCBASED
      case MSR_IA32_VMX_TRUE_EXIT_CTLS:      ActualMsr = 0x483; break; // standard EXIT
      case MSR_IA32_VMX_TRUE_ENTRY_CTLS:     ActualMsr = 0x484; break; // standard ENTRY
    }
  }

  UINT64 Msr = AsmReadMsr64(ActualMsr);
  UINT32 Mandatory1s = (UINT32)(Msr & 0xFFFFFFFF); // must be 1
  UINT32 Allowed1s = (UINT32)(Msr >> 32);          // may be 1
  return (Desired | Mandatory1s) & Allowed1s;
}

static UINT64 GetSegmentBase(UINT64 GdtBase, UINT16 Selector) {
  if ((Selector & ~0x7) == 0)
    return 0;
  PGDT_ENTRY E = (PGDT_ENTRY)(GdtBase + (Selector & ~0x7));
  UINT64 Base = ((UINT64)E->BaseLow) | ((UINT64)E->BaseMid << 16) |
                ((UINT64)E->BaseHigh << 24);
  // System descriptor: S-bit clear
  if ((E->Access & 0x10) == 0) {
    PGDT_ENTRY64 E64 = (PGDT_ENTRY64)E;
    Base |= ((UINT64)E64->BaseUpper << 32);
  }
  return Base;
}

static UINT32 GetSegmentLimit(UINT64 GdtBase, UINT16 Selector) {
  if ((Selector & ~0x7) == 0)
    return 0;
  PGDT_ENTRY E = (PGDT_ENTRY)(GdtBase + (Selector & ~0x7));
  UINT32 Limit = ((UINT32)(E->LimitHighFlags & 0x0F) << 16) | E->LimitLow;
  if (E->LimitHighFlags & 0x80)
    Limit = (Limit << 12) | 0xFFF;
  return Limit;
}

static UINT32 GetSegmentAr(UINT64 GdtBase, UINT16 Selector) {
  if ((Selector & ~0x7) == 0)
    return 0x10000; // Unusable
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
// Full 512GB identity map matching EPT coverage.
// Memory: 1 PML4 + 1 PDPT + 512 PD = 514 pages (~2MB)
// ============================================================
static UINT64 BuildHostPageTable(VOID) {
  // PML4
  EFI_PHYSICAL_ADDRESS Pml4 = MemAllocatePages(1);
  if (!Pml4)
    return 0;
  SetMem((VOID *)Pml4, 4096, 0);

  // PDPT
  EFI_PHYSICAL_ADDRESS Pdpt = MemAllocatePages(1);
  if (!Pdpt)
    return 0;
  SetMem((VOID *)Pdpt, 4096, 0);

// 512 PDs (each covers 1GB via 512 x 2MB pages)
#define HOST_PDPT_ENTRIES 512
  EFI_PHYSICAL_ADDRESS Pd[HOST_PDPT_ENTRIES];
  for (UINTN i = 0; i < HOST_PDPT_ENTRIES; i++) {
    Pd[i] = MemAllocatePages(1);
    if (!Pd[i])
      return 0;
    SetMem((VOID *)Pd[i], 4096, 0);
  }

  // PML4[0] -> PDPT (P, R/W)
  *((UINT64 *)Pml4) = Pdpt | 0x3;

  // PDPT[0..511] -> PDs (P, R/W)
  UINT64 *PdptE = (UINT64 *)Pdpt;
  for (UINTN i = 0; i < HOST_PDPT_ENTRIES; i++) {
    PdptE[i] = Pd[i] | 0x3;
  }

  // Fill each PD: 512 x 2MB large pages (P, R/W, PS)
  for (UINTN i = 0; i < HOST_PDPT_ENTRIES; i++) {
    UINT64 *PdE = (UINT64 *)Pd[i];
    for (UINTN j = 0; j < 512; j++) {
      UINT64 PhysFrame = (UINT64)(i * 512 + j); // 2MB frame index
      PdE[j] = (PhysFrame << 21) | 0x83;        // P + R/W + PS
    }
  }

  return (UINT64)Pml4;
}

// Helper to find TSS selector in GDT if TR is 0
static UINT16 FindTssSelector(UINT64 GdtBase, UINT16 GdtLimit) {
  for (UINT16 i = 8; i < GdtLimit; i += 8) {
    PGDT_ENTRY E = (PGDT_ENTRY)(GdtBase + i);
    // S-bit (bit 4) must be 0 for system descriptors
    if ((E->Access & 0x10) == 0) {
      UINT8 Type = E->Access & 0x0F;
      if (Type == 9 || Type == 11)
        return i; // 64-bit TSS
    }
    // If it's a 64-bit system descriptor, it takes 16 bytes
    if ((E->Access & 0x10) == 0)
      i += 8;
  }
  return 0;
}

EFI_PHYSICAL_ADDRESS g_MsrBitmap = 0;
EFI_PHYSICAL_ADDRESS g_HandlerDest = 0;
UINT64 g_HostCr3 = 0;

BOOLEAN PrepareSharedResources(VOID) {
  // 1. Allocate & Setup MSR Bitmap (filled with 0 = no MSRs cause exits)
  g_MsrBitmap = MemAllocatePages(1);
  if (!g_MsrBitmap) {
    ComPrint("[!] Shared MSR Bitmap alloc failed.\r\n");
    return FALSE;
  }
  SetMem((VOID *)g_MsrBitmap, 4096, 0);
  VmxSetMsrBitmap((VOID *)g_MsrBitmap, 0x3A, TRUE, FALSE);
  VmxSetMsrBitmap((VOID *)g_MsrBitmap, 0x1D9, TRUE, TRUE);
  VmxSetMsrBitmap((VOID *)g_MsrBitmap, 0x570, TRUE, TRUE);
  VmxSetMsrBitmap((VOID *)g_MsrBitmap, 0xC0000082, FALSE, TRUE);

  // 2. Copy VM-Exit handler stub to heap
  #define HANDLER_HEADER_SIZE 128
  UINTN HandlerSize = (UINTN)MinimalVmexitHandlerEnd - (UINTN)MinimalVmexitHandlerStart;
  g_HandlerDest = MemAllocatePages(3);   // 3 pages = 12 KB; large enough for handler + helpers
  if (!g_HandlerDest) {
    ComPrint("[!] Shared VM-Exit handler alloc failed.\r\n");
    return FALSE;
  }

  // Allocate 4KB circular exit-reason ring buffer
  EFI_PHYSICAL_ADDRESS DebugLogPage = MemAllocatePages(1);
  if (!DebugLogPage) {
    ComPrint("[!] Shared Debug log page alloc failed.\r\n");
    return FALSE;
  }

  *((UINT64 *)(g_HandlerDest + 0)) = (UINT64)DebugLogPage;
  *((UINT64 *)(g_HandlerDest + 8)) = 0ULL;
  *((UINT64 *)(g_HandlerDest + 16)) = 0ULL; // virtual DEBUGCTL
  *((UINT64 *)(g_HandlerDest + 24)) = 0ULL; // virtual RTIT_CTL
  *((UINT64 *)(g_HandlerDest + 32)) = 0ULL; // LSTAR
  *((UINT64 *)(g_HandlerDest + 40)) = 0ULL; // authenticated CR3
  *((UINT64 *)(g_HandlerDest + 48)) = 0ULL; // backdoor flags
  *((UINT64 *)(g_HandlerDest + 56)) = 0ULL; // reserved

  // Store Framebuffer info for screen logging
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID**)&Gop);
  if (Gop && Gop->Mode) {
      *((UINT64 *)(g_HandlerDest + 64)) = Gop->Mode->FrameBufferBase;
      *((UINT32 *)(g_HandlerDest + 72)) = Gop->Mode->Info->HorizontalResolution;
      *((UINT32 *)(g_HandlerDest + 76)) = Gop->Mode->Info->VerticalResolution;
      *((UINT32 *)(g_HandlerDest + 80)) = Gop->Mode->Info->PixelsPerScanLine;
      *((UINT32 *)(g_HandlerDest + 84)) = 0; // Current X cursor
      *((UINT32 *)(g_HandlerDest + 88)) = 0; // Current Y cursor
  } else {
      *((UINT64 *)(g_HandlerDest + 64)) = 0;
  }

  CopyMem((VOID *)(g_HandlerDest + HANDLER_HEADER_SIZE),
          (VOID *)MinimalVmexitHandlerStart, HandlerSize);

  ComPrint("[*] Shared Handler base: 0x");
  ComPrintHex((UINT64)g_HandlerDest);
  ComPrint("  HandlerSize: 0x");
  ComPrintHex((UINT64)HandlerSize);
  ComPrint("  Shared DebugLog: 0x");
  ComPrintHex((UINT64)DebugLogPage);
  ComPrint("\r\n");

  // 3. Build a private 4-level identity-map host page table (PML4 → PDPT → 512 PDs of 2MB pages).
  //
  // We CANNOT reuse the UEFI boot-services CR3 here.  winload.efi invalidates
  // every boot-services page immediately after ExitBootServices returns, so the
  // first VM-exit after the OS takes control would triple-fault when the CPU
  // tries to walk a now-gone UEFI page table.
  //
  // All allocations go through MemAllocatePages which pulls from the 8MB
  // EfiRuntimeServicesData region allocated at startup — memory that Windows
  // leaves completely intact forever.

  // PML4 (1 page)
  EFI_PHYSICAL_ADDRESS HostPml4 = MemAllocatePages(1);
  if (!HostPml4) { ComPrint("[!] Host PML4 alloc failed.\r\n"); return FALSE; }
  SetMem((VOID *)HostPml4, 4096, 0);

  // PDPT (1 page, covers 512 × 1GB = 512GB)
  EFI_PHYSICAL_ADDRESS HostPdpt = MemAllocatePages(1);
  if (!HostPdpt) { ComPrint("[!] Host PDPT alloc failed.\r\n"); return FALSE; }
  SetMem((VOID *)HostPdpt, 4096, 0);

  // 512 PDs, each mapping 512 × 2MB = 1GB
  #define HOST_PT_PDPT_ENTRIES 512
  EFI_PHYSICAL_ADDRESS HostPd[HOST_PT_PDPT_ENTRIES];
  for (UINTN i = 0; i < HOST_PT_PDPT_ENTRIES; i++) {
    HostPd[i] = MemAllocatePages(1);
    if (!HostPd[i]) { ComPrint("[!] Host PD alloc failed.\r\n"); return FALSE; }
    SetMem((VOID *)HostPd[i], 4096, 0);
  }

  // Wire PML4[0] → PDPT  (bit 0=P, bit 1=RW, bit 2=US, bit 3=PWT, bit 4=PCD, bit 5=A)
  UINT64 *Pml4e = (UINT64 *)HostPml4;
  Pml4e[0] = HostPdpt | 0x3;  // P + RW

  // Wire PDPT[i] → PD[i] and fill PD[i] with 512 × 2MB identity pages
  UINT64 *Pdpte = (UINT64 *)HostPdpt;
  for (UINTN i = 0; i < HOST_PT_PDPT_ENTRIES; i++) {
    Pdpte[i] = HostPd[i] | 0x3;  // P + RW

    UINT64 *Pde = (UINT64 *)HostPd[i];
    for (UINTN j = 0; j < 512; j++) {
      UINT64 Phys2MB = ((UINT64)(i * 512 + j)) << 21; // 2MB aligned GPA
      // bit 7 = PS (2MB page), bit 1 = RW, bit 0 = P
      Pde[j] = Phys2MB | 0x83;  // P + RW + PS(large)
    }
  }

  g_HostCr3 = HostPml4;

  ComPrint("[*] Private Host CR3 built: 0x");
  ComPrintHex(g_HostCr3);
  ComPrint("\r\n");

  return TRUE;
}

BOOLEAN InitializeVmcsPerCore(
  IN EFI_PHYSICAL_ADDRESS VmcsPhys,
  IN EFI_PHYSICAL_ADDRESS GdtDest,
  IN EFI_PHYSICAL_ADDRESS TssPhys,
  IN EFI_PHYSICAL_ADDRESS HostStackTop,
  IN EFI_PHYSICAL_ADDRESS Ist1StackTop,
  IN EFI_PHYSICAL_ADDRESS IdtDest,
  IN UINT64 HostCr3,
  IN EFI_PHYSICAL_ADDRESS MsrBitmap,
  IN EFI_PHYSICAL_ADDRESS HandlerDest
  ) 
{
  if (!VmcsPhys || !GdtDest || !TssPhys || !HostStackTop || !Ist1StackTop || !IdtDest || !HostCr3 || !MsrBitmap || !HandlerDest) {
    ComPrint("[!] InitializeVmcsPerCore: Null parameter passed.\r\n");
    return FALSE;
  }

  PVMCS_REGION Vmcs = (PVMCS_REGION)VmcsPhys;
  UINT64 VmxBasic = AsmReadMsr64(MSR_IA32_VMX_BASIC);
  Vmcs->RevisionId = (UINT32)(VmxBasic & 0x7FFFFFFF);

  if (__vmx_vmclear((unsigned long long *)&VmcsPhys) != 0) {
    ComPrint("[!] VMCLEAR failed.\r\n");
    return FALSE;
  }
  if (__vmx_vmptrld((unsigned long long *)&VmcsPhys) != 0) {
    ComPrint("[!] VMPTRLD failed.\r\n");
    return FALSE;
  }

  UINTN Cr0 = AsmReadCr0();
  UINTN Cr3 = AsmReadCr3();
  UINTN Cr4 = AsmReadCr4();

  IA32_DESCRIPTOR Gdtr, Idtr;
  AsmReadGdtr(&Gdtr);
  AsmReadIdtr(&Idtr);
  UINT16 CsSel = AsmReadCs();
  UINT16 SsSel = AsmReadSs();
  UINT16 DsSel = AsmReadDs();
  UINT16 EsSel = AsmReadEs();
  UINT64 GdtBase = Gdtr.Base;

  // Synthesize a valid stack segment (SS) selector if it is null in UEFI (common on physical hardware)
  if ((SsSel & ~0x7) == 0) {
    if ((DsSel & ~0x7) != 0) {
      SsSel = DsSel;
    } else if ((EsSel & ~0x7) != 0) {
      SsSel = EsSel;
    } else {
      for (UINT16 Selector = 8; Selector < Gdtr.Limit; Selector += 8) {
        PGDT_ENTRY Entry = (PGDT_ENTRY)(GdtBase + Selector);
        // S-bit (bit 4) = 1 (code/data), Executable bit 3 = 0 (data segment)
        if ((Entry->Access & 0x10) != 0 && (Entry->Access & 0x08) == 0) {
          SsSel = Selector;
          break;
        }
      }
    }
    // If still null, fallback to standard flat data selector
    if ((SsSel & ~0x7) == 0) {
      SsSel = 0x30; // standard UEFI 64-bit flat data selector
    }
  }

  // --- VMX TR Synthesis & GDT Relocation (HyperVenom style) ---
  // Copy current core's GDT to the preallocated destination
  CopyMem((VOID *)GdtDest, (VOID *)Gdtr.Base, Gdtr.Limit + 1);

  // Initialize 64-bit TSS structure on this core's preallocated TSS page
  typedef struct {
      UINT32 Reserved0;
      UINT64 RSP0;        // Stack for CPL 0
      UINT64 RSP1;        // Stack for CPL 1
      UINT64 RSP2;        // Stack for CPL 2
      UINT64 Reserved1;
      UINT64 IST1;        // Interrupt Stack Table 1 (for Double Fault)
      UINT64 IST2;
      UINT64 IST3;
      UINT64 IST4;
      UINT64 IST5;
      UINT64 IST6;
      UINT64 IST7;
      UINT64 Reserved2;
      UINT16 Reserved3;
      UINT16 IOMapBase;
  } TSS64;

  TSS64* Tss = (TSS64*)TssPhys;
  Tss->RSP0 = HostStackTop;      // Use per-core host stack
  Tss->IST1 = Ist1StackTop;      // Use per-core IST1 stack
  Tss->IOMapBase = sizeof(TSS64); // No I/O permission bitmap

  // --- Dynamic TSS placement: append at end of GDT (never overwrite existing entries) ---
  // Align TSS selector to next 16-byte boundary after the existing GDT content
  // A 64-bit TSS descriptor takes 16 bytes (two 8-byte GDT slots)
  UINT16 TrSel = (Gdtr.Limit + 1 + 15) & ~15; // Round up to 16-byte alignment
  if ((UINTN)TrSel + 16 > 4096) TrSel = 0x40;  // Fallback: shouldn't happen

  PGDT_ENTRY64 TssDesc = (PGDT_ENTRY64)((UINT8 *)GdtDest + TrSel);
  TssDesc->LimitLow = 0x67;
  TssDesc->BaseLow = (UINT16)(TssPhys & 0xFFFF);
  TssDesc->BaseMid = (UINT8)((TssPhys >> 16) & 0xFF);
  TssDesc->Access = 0x8B; // Present, Busy 64-bit TSS (Type 11)
  TssDesc->LimitHighFlags = 0x00;
  TssDesc->BaseHigh = (UINT8)((TssPhys >> 24) & 0xFF);
  TssDesc->BaseUpper = (UINT32)(TssPhys >> 32);
  TssDesc->Reserved = 0;

  UINT32 GuestTrLimit = 0x67;
  UINT32 GuestTrAr = 0x008B; // Present, DPL 0, Busy 64-bit TSS
  UINT64 GuestTrBase = TssPhys;

  // New GDT limit covers our TSS descriptor (TrSel + 15 = last byte of 16-byte TSS entry)
  UINT16 NewGdtLimit = TrSel + 15;

  // 3. Set VM-Execution Controls
  UINT32 PinCtls = AdjustVmxControl(0, MSR_IA32_VMX_TRUE_PINBASED_CTLS);
  UINT32 ProcCtls =
      AdjustVmxControl(CPU_SECONDARY_CONTROLS | CPU_BASED_USE_MSR_BITMAPS,
                       MSR_IA32_VMX_TRUE_PROCBASED_CTLS);

  // Enable EPT (bit 1) + RDTSCP (bit 3) + INVPCID (bit 12) + Unrestricted Guest (bit 7).
  // INVPCID is required: without it, Windows kernel INVPCID causes #UD crash.
  // XSAVES/XRSTORS (bit 20) omitted - needs extra IA32_XSS configuration.
  #define SECONDARY_ENABLE_UNRESTRICTED_GUEST (1u << 7)
  UINT32 Proc2Ctls = AdjustVmxControl(
      0x00000002 | SECONDARY_ENABLE_RDTSCP | SECONDARY_ENABLE_INVPCID | SECONDARY_ENABLE_UNRESTRICTED_GUEST,
      MSR_IA32_VMX_PROCBASED_CTLS2);

  UINT32 ExitCtls =
      AdjustVmxControl(VMEXIT_HOST_ADDR_SPACE_SIZE | VMEXIT_LOAD_IA32_EFER | VMEXIT_SAVE_IA32_EFER,
                         MSR_IA32_VMX_TRUE_EXIT_CTLS);
  UINT32 EntryCtls =
      AdjustVmxControl(VMENTRY_IA32E_GUEST | VMENTRY_LOAD_IA32_EFER,
                         MSR_IA32_VMX_TRUE_ENTRY_CTLS);

  // Write EPTP to VMCS
  extern EPTP g_EptPointer;
  __vmx_vmwrite(EPT_POINTER, g_EptPointer.All);

  // Set preallocated MSR Bitmap
  __vmx_vmwrite(MSR_BITMAP, MsrBitmap);

  __vmx_vmwrite(PIN_BASED_VM_EXEC_CONTROL, PinCtls);
  __vmx_vmwrite(CPU_BASED_VM_EXEC_CONTROL, ProcCtls);
  __vmx_vmwrite(SECONDARY_VM_EXEC_CONTROL, Proc2Ctls);
  __vmx_vmwrite(VM_EXIT_CONTROLS, ExitCtls);
  __vmx_vmwrite(VM_ENTRY_CONTROLS, EntryCtls);
  
  // Intercept #UD (Invalid Opcode, bit 6)
  __vmx_vmwrite(EXCEPTION_BITMAP, (1 << 6));

  // CR0/CR4 Shadows and Masks to prevent #GP on guest writes
  __vmx_vmwrite(CR0_GUEST_HOST_MASK, 0); // Allow guest full control of CR0
  __vmx_vmwrite(CR4_GUEST_HOST_MASK,
                0x2000); // Mask VMXE (bit 13) to prevent guest GP on CR4 writes
  __vmx_vmwrite(CR0_READ_SHADOW, Cr0);
  __vmx_vmwrite(CR4_READ_SHADOW, Cr4 & ~0x2000); // Hide VMXE from guest

  // 4. Guest State
  EsSel = AsmReadEs();
  DsSel = AsmReadDs();
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

  __vmx_vmwrite(GUEST_CS_LIMIT, GetSegmentLimit(GdtBase, CsSel));
  // SS limit: use 0xFFFFFFFF when selector is synthesized (limit may not be in GDT)
  UINT32 SsLimit = GetSegmentLimit(GdtBase, SsSel);
  if (SsLimit == 0) {
    SsLimit = 0xFFFFFFFF;
  }
  __vmx_vmwrite(GUEST_SS_LIMIT, SsLimit);
  __vmx_vmwrite(GUEST_DS_LIMIT, GetSegmentLimit(GdtBase, DsSel));
  __vmx_vmwrite(GUEST_ES_LIMIT, GetSegmentLimit(GdtBase, EsSel));
  __vmx_vmwrite(GUEST_FS_LIMIT, GetSegmentLimit(GdtBase, FsSel));
  __vmx_vmwrite(GUEST_GS_LIMIT, GetSegmentLimit(GdtBase, GsSel));
  __vmx_vmwrite(GUEST_TR_LIMIT, GuestTrLimit);
  __vmx_vmwrite(GUEST_LDTR_LIMIT, 0xFFFF);
  __vmx_vmwrite(GUEST_GDTR_LIMIT, NewGdtLimit);
  __vmx_vmwrite(GUEST_IDTR_LIMIT, Idtr.Limit);

  __vmx_vmwrite(GUEST_CS_AR, GetSegmentAr(GdtBase, CsSel));

  // GUEST_SS_AR: Intel requires SS to be a usable writable data segment.
  // If AR comes back as 0x10000 (Unusable) or 0, force a valid flat data segment AR.
  UINT32 SsAr = GetSegmentAr(GdtBase, SsSel);
  if (SsAr == 0x10000 || SsAr == 0) {
    SsAr = 0x0093; // Present, DPL0, S=1 (code/data), Type=3 (writable data), 64-bit
  }
  __vmx_vmwrite(GUEST_SS_AR, SsAr);

  __vmx_vmwrite(GUEST_DS_AR, GetSegmentAr(GdtBase, DsSel));
  __vmx_vmwrite(GUEST_ES_AR, GetSegmentAr(GdtBase, EsSel));
  __vmx_vmwrite(GUEST_FS_AR, GetSegmentAr(GdtBase, FsSel));
  __vmx_vmwrite(GUEST_GS_AR, GetSegmentAr(GdtBase, GsSel));
  __vmx_vmwrite(GUEST_TR_AR, GuestTrAr);
  __vmx_vmwrite(GUEST_LDTR_AR, 0x10000);

  __vmx_vmwrite(GUEST_CS_BASE, GetSegmentBase(GdtBase, CsSel));
  __vmx_vmwrite(GUEST_SS_BASE, GetSegmentBase(GdtBase, SsSel));
  __vmx_vmwrite(GUEST_DS_BASE, GetSegmentBase(GdtBase, DsSel));
  __vmx_vmwrite(GUEST_ES_BASE, GetSegmentBase(GdtBase, EsSel));
  __vmx_vmwrite(GUEST_FS_BASE, AsmReadMsr64(MSR_IA32_FS_BASE));
  __vmx_vmwrite(GUEST_GS_BASE, AsmReadMsr64(MSR_IA32_GS_BASE));
  __vmx_vmwrite(GUEST_TR_BASE, GuestTrBase);
  __vmx_vmwrite(GUEST_LDTR_BASE, 0);
  __vmx_vmwrite(GUEST_GDTR_BASE, GdtDest); // Relocated GDT containing TSS descriptor
  __vmx_vmwrite(GUEST_IDTR_BASE, Idtr.Base);

  __vmx_vmwrite(GUEST_CR0, Cr0);
  __vmx_vmwrite(GUEST_CR3, Cr3);
  __vmx_vmwrite(GUEST_CR4, Cr4);
  __vmx_vmwrite(GUEST_DR7, 0x400);
  __vmx_vmwrite(GUEST_RFLAGS, AsmReadEflags()); // exact system flags

  __vmx_vmwrite(GUEST_SYSENTER_CS, AsmReadMsr64(MSR_IA32_SYSENTER_CS));
  __vmx_vmwrite(GUEST_SYSENTER_ESP, AsmReadMsr64(MSR_IA32_SYSENTER_ESP));
  __vmx_vmwrite(GUEST_SYSENTER_EIP, AsmReadMsr64(MSR_IA32_SYSENTER_EIP));
  __vmx_vmwrite(GUEST_DEBUGCTL, AsmReadMsr64(MSR_IA32_DEBUGCTL));
  __vmx_vmwrite(GUEST_EFER, AsmReadMsr64(MSR_IA32_EFER));
  __vmx_vmwrite(GUEST_VMCS_LINK_PTR, 0xFFFFFFFFFFFFFFFF);

  __vmx_vmwrite(GUEST_ACTIVITY_STATE, 0);
  __vmx_vmwrite(GUEST_INTERRUPTIBILITY, 0);
  __vmx_vmwrite(GUEST_RSP, 0);
  __vmx_vmwrite(GUEST_RIP, 0);

  // 5. Host State
  __vmx_vmwrite(HOST_CS_SELECTOR, CsSel & ~0x7);
  __vmx_vmwrite(HOST_SS_SELECTOR, SsSel & ~0x7);
  __vmx_vmwrite(HOST_DS_SELECTOR, SsSel & ~0x7); // Use SS
  __vmx_vmwrite(HOST_ES_SELECTOR, SsSel & ~0x7); // Use SS
  __vmx_vmwrite(HOST_FS_SELECTOR, SsSel & ~0x7); // Use SS
  __vmx_vmwrite(HOST_GS_SELECTOR, SsSel & ~0x7); // Use SS

  __vmx_vmwrite(HOST_TR_SELECTOR, TrSel & ~0x7);

  __vmx_vmwrite(HOST_CR0, Cr0);
  __vmx_vmwrite(HOST_CR3, HostCr3); // pre-built runtime host-CR3
  __vmx_vmwrite(HOST_CR4, Cr4);

  __vmx_vmwrite(HOST_FS_BASE, AsmReadMsr64(MSR_IA32_FS_BASE));
  __vmx_vmwrite(HOST_GS_BASE, AsmReadMsr64(MSR_IA32_GS_BASE));
  __vmx_vmwrite(HOST_TR_BASE, GuestTrBase);
  __vmx_vmwrite(HOST_GDTR_BASE, GdtDest);
  
  // Copy IDT to the preallocated destination page
  CopyMem((VOID *)IdtDest, (VOID *)Idtr.Base, Idtr.Limit + 1);
  __vmx_vmwrite(HOST_IDTR_BASE, IdtDest);

  // HOST_RIP = HandlerDest + 128 (skip the 128-byte state header)
  __vmx_vmwrite(HOST_RSP, HostStackTop);
  __vmx_vmwrite(HOST_RIP, HandlerDest + 128);

  __vmx_vmwrite(HOST_SYSENTER_CS, AsmReadMsr64(MSR_IA32_SYSENTER_CS));
  __vmx_vmwrite(HOST_SYSENTER_ESP, AsmReadMsr64(MSR_IA32_SYSENTER_ESP));
  __vmx_vmwrite(HOST_SYSENTER_EIP, AsmReadMsr64(MSR_IA32_SYSENTER_EIP));
  __vmx_vmwrite(HOST_EFER, AsmReadMsr64(MSR_IA32_EFER));

  return TRUE;
}
