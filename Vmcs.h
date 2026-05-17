#ifndef VMCS_H
#define VMCS_H

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>

BOOLEAN InitializeVmcs(VOID);

// Assembly: sets Guest RSP/RIP from call stack then executes VMLAUNCH.
// After VMLAUNCH succeeds, guest resumes at the caller's return site.
// Returns 0 on failure.
UINT64 AsmVmlaunchAndCaptureState(VOID);

// Position-independent minimal VM-exit stub (to be copied to runtime heap)
extern VOID MinimalVmexitHandlerStart(VOID);
extern VOID MinimalVmexitHandlerEnd(VOID);

#endif // VMCS_H
