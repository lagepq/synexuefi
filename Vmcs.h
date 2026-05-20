#ifndef VMCS_H
#define VMCS_H

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>

extern EFI_PHYSICAL_ADDRESS g_MsrBitmap;
extern EFI_PHYSICAL_ADDRESS g_HandlerDest;
extern UINT64 g_HostCr3;

BOOLEAN PrepareSharedResources(VOID);

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
  );

// Assembly: sets Guest RSP/RIP from call stack then executes VMLAUNCH.
// After VMLAUNCH succeeds, guest resumes at the caller's return site.
// Returns 0 on failure.
UINT64 AsmVmlaunchAndCaptureState(VOID);

// Position-independent minimal VM-exit stub (to be copied to runtime heap)
extern VOID MinimalVmexitHandlerStart(VOID);
extern VOID MinimalVmexitHandlerEnd(VOID);

#endif // VMCS_H
