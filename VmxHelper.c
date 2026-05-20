#include "VmxHelper.h"
#include "PhysicalMemory.h"
#include "ComLogger.h"
#include <Library/BaseLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <intrin.h>


BOOLEAN IsVmxSupported(VOID)
{
    UINT32 Eax, Ebx, Ecx, Edx;
    
    // Check CPUID leaf 1, ECX bit 5
    AsmCpuid(CPUID_VERSION_INFO, &Eax, &Ebx, &Ecx, &Edx);
    if ((Ecx & CPUID_FEAT_ECX_VMX) == 0) {
        ComPrint("[!] VMX is not supported by CPU (CPUID).\r\n");
        return FALSE;
    }
    
    // Check IA32_FEATURE_CONTROL MSR
    UINT64 FeatureControl = AsmReadMsr64(MSR_IA32_FEATURE_CONTROL);
    if ((FeatureControl & 1) == 0) {
        // Lock bit is 0, VMX is disabled in BIOS
        ComPrint("[!] VMX is disabled in BIOS (Lock bit 0).\r\n");
        return FALSE;
    }
    
    if ((FeatureControl & 4) == 0) {
        // Bit 2 (Enable VMX outside SMX) is 0
        ComPrint("[!] VMX is disabled in BIOS (Enable bit 0).\r\n");
        return FALSE;
    }


    return TRUE;
}

BOOLEAN EnableVmxOperation(IN BOOLEAN Verbose)
{
    // 1. Configure CR0 using Intel fixed MSRs (forcing CR0.NE=1, CR0.PG=1, CR0.PE=1)
    UINT64 Cr0 = AsmReadCr0();
    UINT64 Cr0Fixed0 = AsmReadMsr64(0x486); // IA32_VMX_CR0_FIXED0
    UINT64 Cr0Fixed1 = AsmReadMsr64(0x487); // IA32_VMX_CR0_FIXED1
    Cr0 = (Cr0 | Cr0Fixed0) & Cr0Fixed1;
    AsmWriteCr0(Cr0);
    if (Verbose) {
        ComPrint("[SynexHV] CR0 configured with VMX fixed bits. CR0 = 0x");
        ComPrintHex(Cr0);
        ComPrint("\r\n");
    }

    // 2. Configure CR4 using Intel fixed MSRs (forcing CR4.VMXE=1)
    UINT64 Cr4 = AsmReadCr4();
    Cr4 |= CR4_VMXE;
    Cr4 |= (1ULL << 18);  // CR4.OSXSAVE — required for XSETBV in VMX-root mode
    UINT64 Cr4Fixed0 = AsmReadMsr64(0x488); // IA32_VMX_CR4_FIXED0
    UINT64 Cr4Fixed1 = AsmReadMsr64(0x489); // IA32_VMX_CR4_FIXED1
    Cr4 = (Cr4 | Cr4Fixed0) & Cr4Fixed1;
    AsmWriteCr4(Cr4);
    if (Verbose) {
        ComPrint("[SynexHV] CR4 configured with VMX fixed bits. CR4 = 0x");
        ComPrintHex(Cr4);
        ComPrint("\r\n");
    }

    return TRUE;
}

BOOLEAN InitializeVmxon(IN EFI_PHYSICAL_ADDRESS VmxonPhysAddr, IN BOOLEAN Verbose)
{
    if (VmxonPhysAddr == 0) {
        if (Verbose) ComPrint("[!] Invalid VMXON physical address.\r\n");
        return FALSE;
    }
    SetMem((VOID*)VmxonPhysAddr, 4096, 0);
    
    if (Verbose) {
        ComPrint("[SynexHV] VMXON region setup at physical address: 0x");
        ComPrintHex(VmxonPhysAddr);
        ComPrint("\r\n");
    }
    
    PVMCS_REGION VmxonRegion = (PVMCS_REGION)VmxonPhysAddr;
    
    // 2. Get the VMX Revision ID from IA32_VMX_BASIC MSR
    UINT64 VmxBasic = AsmReadMsr64(MSR_IA32_VMX_BASIC);
    UINT32 RevisionId = (UINT32)(VmxBasic & 0x7FFFFFFF); // Bits 0:30
    
    if (Verbose) {
        ComPrint("[SynexHV] IA32_VMX_BASIC basic details: 0x");
        ComPrintHex(VmxBasic);
        ComPrint("\r\n");
        
        ComPrint("[SynexHV] Target VMX Revision ID: 0x");
        ComPrintHex(RevisionId);
        ComPrint("\r\n");
    }
    
    VmxonRegion->RevisionId = RevisionId;
    
    UINT64 FeatureControl = AsmReadMsr64(MSR_IA32_FEATURE_CONTROL);
    if (Verbose) {
        ComPrint("[SynexHV] IA32_FEATURE_CONTROL MSR: 0x");
        ComPrintHex(FeatureControl);
        ComPrint("\r\n");
        
        // 3. Execute VMXON using our robust, compiler-safe assembly wrapper
        ComPrint("[SynexHV] Executing AsmVmxon instruction now...\r\n");
    }
    
    UINT8 status = AsmVmxon((UINT64)VmxonPhysAddr);
    
    if (status != 0) {
        if (Verbose) {
            ComPrint("[!] VMXON failed with status code: ");
            ComPrintHex(status);
            if (status == 1) {
                ComPrint(" (VMXON failed with Carry Flag set - VMXON pointer invalid or out of range)\r\n");
            } else if (status == 2) {
                ComPrint(" (VMXON failed with Zero Flag set - VMXON failed outside VMXON pointer validation)\r\n");
            } else {
                ComPrint("\r\n");
            }
        }
        return FALSE;
    }

    if (Verbose) ComPrint("[+] VMXON executed successfully on current core. VMX Root Mode active!\r\n");
    return TRUE;
}

VOID VmxSetMsrBitmap(VOID* Bitmap, UINT32 Msr, BOOLEAN Read, BOOLEAN Write)
{
    UINT8* BitmapBytes = (UINT8*)Bitmap;
    UINT32 ByteOffset;
    UINT8 BitMask;

    if (Msr <= 0x1FFF) {
        // Low MSRs (0x00000000 - 0x00001FFF)
        ByteOffset = Msr / 8;
        BitMask = 1 << (Msr % 8);

        if (Read)  BitmapBytes[ByteOffset + 0x000] |= BitMask;  // Read Low
        if (Write) BitmapBytes[ByteOffset + 0x800] |= BitMask;  // Write Low
    } 
    else if (Msr >= 0xC0000000 && Msr <= 0xC0001FFF) {
        // High MSRs (0xC0000000 - 0xC0001FFF)
        UINT32 LowMsr = Msr - 0xC0000000;
        ByteOffset = LowMsr / 8;
        BitMask = 1 << (LowMsr % 8);

        if (Read)  BitmapBytes[ByteOffset + 0x400] |= BitMask;  // Read High
        if (Write) BitmapBytes[ByteOffset + 0xC00] |= BitMask;  // Write High
    }
}

