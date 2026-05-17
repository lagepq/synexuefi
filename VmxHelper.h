#ifndef VMX_HELPER_H
#define VMX_HELPER_H

#include <Uefi.h>

// Intel CPUID and MSR definitions
#define CPUID_VERSION_INFO          0x01
#define CPUID_FEAT_ECX_VMX          (1 << 5)

#define MSR_IA32_FEATURE_CONTROL    0x3A
#define MSR_IA32_VMX_BASIC          0x480

// CR4 VMX enable bit
#define CR4_VMXE                    (1 << 13)

#pragma pack(push, 1)
typedef struct _VMCS_REGION {
    UINT32 RevisionId;
    UINT32 AbortIndicator;
    UINT8  Data[0x1000 - 8];
} VMCS_REGION, *PVMCS_REGION;
#pragma pack(pop)

// Main initialization functions
UINT8 AsmVmxon(UINT64 VmxonPhysAddr);
BOOLEAN IsVmxSupported(VOID);
BOOLEAN EnableVmxOperation(VOID);
BOOLEAN InitializeVmxon(VOID);
VOID VmxSetMsrBitmap(VOID* Bitmap, UINT32 Msr, BOOLEAN Read, BOOLEAN Write);


#endif // VMX_HELPER_H
