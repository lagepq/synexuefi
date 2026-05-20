#include "VmexitDispatcher.h"
#include "GuestRegisters.h"
#include "ComLogger.h"
#include <Library/BaseLib.h>
#include <intrin.h>
#include "PhysicalMemory.h"
#include "Ept.h"


// Global authenticated state
static UINT64 g_AuthenticatedCr3 = 0;
static BOOLEAN g_BackdoorEnabled = FALSE;
static UINT64 g_KernelBase = 0;

// Virtual MSR values (simplified for 1 core)
static UINT64 g_VirtualDebugCtl = 0;
static UINT64 g_VirtualRtitCtl = 0;



// Returns 1 to VMRESUME, 0 to pass to original handler (for future use)
BOOLEAN VmexitDispatcher(PGUEST_REGISTERS GuestRegs)
{
    UINTN ExitReason;
    UINTN ExitInstructionLength;
    UINT64 GuestRip;

    __vmx_vmread(VM_EXIT_REASON, &ExitReason);
    __vmx_vmread(VM_EXIT_INSTRUCTION_LEN, &ExitInstructionLength);
    __vmx_vmread(GUEST_RIP, &GuestRip);

    // Optional: Log exits (can be very spammy, but useful for now)
    // ComPrint("[SynexHV] VM-Exit Reason: ");
    // ComPrintHex(ExitReason & 0xFFFF);
    // ComPrint("\r\n");

    switch (ExitReason & 0xFFFF) {

    case EXIT_REASON_CPUID:
    {
        // Execute real CPUID with guest leaf/subleaf
        UINT32 Leaf    = (UINT32)GuestRegs->Rax;
        UINT32 Subleaf = (UINT32)GuestRegs->Rcx;
        UINT32 Regs[4] = {0};
        
        __cpuidex((int*)Regs, (int)Leaf, (int)Subleaf);

        // --- SYNEX BACKDOOR START ---
        if (Leaf == SYNEX_BACKDOOR_LEAF && GuestRegs->Rdx == SYNEX_BACKDOOR_MAGIC) {
            UINT64 GuestCs;
            __vmx_vmread(GUEST_CS_SELECTOR, &GuestCs);

            // CPL check (low 2 bits of selector)
            if ((GuestCs & 3) == 3) {
                UINT64 GuestCr3;
                __vmx_vmread(0x00006802, &GuestCr3); // GUEST_CR3

                // Op 0: Init/Authenticate
                if (Subleaf == 0) {
                    if (g_AuthenticatedCr3 == 0) {
                        g_AuthenticatedCr3 = GuestCr3;
                        g_BackdoorEnabled = TRUE;
                        ComPrint("[SynexHV] Backdoor authenticated for CR3: ");
                        ComPrintHex(GuestCr3);
                        ComPrint("\r\n");
                        GuestRegs->R11 = 0; // Success
                    } else if (g_AuthenticatedCr3 == GuestCr3) {
                        GuestRegs->R11 = 0; // Already auth
                    } else {
                        GuestRegs->R11 = 1; // Locked to another process
                    }
                } 
                // Other Ops: Check CR3 lock
                else if (g_BackdoorEnabled && g_AuthenticatedCr3 == GuestCr3) {
                    switch (Subleaf) {
                        case 1: // TranslateVirtual
                        {
                            UINT64 GVA = GuestRegs->R8;
                            UINT64 GPA = TranslateGuestVirtual(GuestCr3, GVA);
                            GuestRegs->R10 = GPA;
                            GuestRegs->R11 = (GPA != 0) ? 0 : 3; // Success or translation fail
                            break;
                        }
                        case 2: // ReadPhysical (16 bytes)
                        {
                            UINT64 GPA = GuestRegs->R8;
                            if (GPA != 0) {
                                GuestRegs->R10 = *(UINT64*)(GPA);
                                GuestRegs->R9  = *(UINT64*)(GPA + 8);
                                GuestRegs->R11 = 0;
                            } else {
                                GuestRegs->R11 = 4;
                            }
                            break;
                        }
                        case 5: // GetLogBuffer (Share circular log buffer address with usermode)
                        {
                            GuestRegs->R10 = LogGetBufferAddress();
                            GuestRegs->R11 = 0;
                            break;
                        }
                        case 6: // DeployEpHook (Stealth Shadow Hook)

                        {
                            UINT64 TargetGva = GuestRegs->R8;
                            UINT64 DetourGva = GuestRegs->R9;
                            UINT64 OriginalFunc = 0;
                            BOOLEAN HookSuccess = DeployEpHook(GuestCr3, TargetGva, DetourGva, &OriginalFunc);
                            GuestRegs->R10 = OriginalFunc; // Output original function pointer
                            GuestRegs->R11 = HookSuccess ? 0 : 6;
                            break;
                        }
                        case 7: // RemoveEpHook (Stealth Shadow Cleanup)
                        {
                            UINT64 TargetGva = GuestRegs->R8;
                            BOOLEAN RemoveSuccess = RemoveEpHook(GuestCr3, TargetGva);
                            GuestRegs->R11 = RemoveSuccess ? 0 : 7;
                            break;
                        }
                        case 10: // GetKernelBase (Placeholder)
                            GuestRegs->R11 = 0;
                            break;
                        default:
                            GuestRegs->R11 = 0xFF; // Unknown op
                            break;

                    }
                } else {

                    GuestRegs->R11 = 2; // Not authenticated or CR3 mismatch
                }
            } else {
                ComPrint("[SynexHV] Backdoor attempt from non-Ring 3!\r\n");
                // Don't modify regs, let it fall through to normal CPUID behavior
            }
            
            // Advance RIP and resume for backdoor calls
            __vmx_vmwrite(GUEST_RIP, GuestRip + ExitInstructionLength);
            return TRUE;
        }
        // --- SYNEX BACKDOOR END ---

        if (Leaf == CPUID_VERSION_INFO_LEAF) {

            // -----------------------------------------------
            // EAC Anti-VM: Clear Hypervisor Present bit (bit 31)
            Regs[2] &= ~(1u << 31);
            // Also clear VMX capability bit (bit 5) to hide virtualization
            Regs[2] &= ~(1u << 5);
            // -----------------------------------------------
        }
        
        if (Leaf == 7 && Subleaf == 0) {
            // -----------------------------------------------
            // Mask CET_SS (ECX bit 7) and CET_IBT (EDX bit 20)
            // to prevent EXCEPTION_ON_INVALID_STACK (0x1AA) BSOD
            Regs[2] &= ~(1u << 7);
            Regs[3] &= ~(1u << 20);
            // -----------------------------------------------
        }
        
        // Completely block the Hypervisor vendor leaf
        if (Leaf == CPUID_HV_VENDOR_LEAF) {
            Regs[0] = 0;
            Regs[1] = 0;
            Regs[2] = 0;
            Regs[3] = 0;
        }

        // Return CPUID results to guest
        GuestRegs->Rax = Regs[0];
        GuestRegs->Rbx = Regs[1];
        GuestRegs->Rcx = Regs[2];
        GuestRegs->Rdx = Regs[3];

        // Advance guest RIP past the CPUID instruction (2 bytes)
        __vmx_vmwrite(GUEST_RIP, GuestRip + ExitInstructionLength);
        return TRUE;
    }

    case EXIT_REASON_MSR_READ:
    {
        UINT32 Msr = (UINT32)GuestRegs->Rcx;
        UINT64 Value = 0;

        if (Msr == 0x1D9) { // IA32_DEBUGCTL
            Value = g_VirtualDebugCtl;
        } else if (Msr == 0x570) { // IA32_RTIT_CTL
            Value = g_VirtualRtitCtl;
        } else {
            Value = __readmsr(Msr);
        }

        GuestRegs->Rax = Value & 0xFFFFFFFF;
        GuestRegs->Rdx = Value >> 32;

        __vmx_vmwrite(GUEST_RIP, GuestRip + ExitInstructionLength);
        return TRUE;
    }

    case EXIT_REASON_MSR_WRITE:
    {
        UINT32 Msr = (UINT32)GuestRegs->Rcx;
        UINT64 Value = (GuestRegs->Rdx << 32) | (GuestRegs->Rax & 0xFFFFFFFF);

        if (Msr == 0x1D9) { // IA32_DEBUGCTL
            g_VirtualDebugCtl = Value;
            // Sabotage: Clear LBR (bit 0) and BTS (bit 7)
            Value &= ~((1ULL << 0) | (1ULL << 7));
        } else if (Msr == 0x570) { // IA32_RTIT_CTL
            g_VirtualRtitCtl = Value;
            // Sabotage: Clear TraceEn (bit 0)
            Value &= ~(1ULL << 0);
        } else if (Msr == 0xC0000082) { // IA32_LSTAR
            // Capture kernel base (roughMZ search would be better, but let's log it)
            if (g_KernelBase == 0) {
                g_KernelBase = Value; 
                ComPrint("[SynexHV] LSTAR write caught. Guest Kernel Syscall Entry: ");
                ComPrintHex(Value);
                ComPrint("\r\n");
            }
        }

        __writemsr(Msr, Value);
        __vmx_vmwrite(GUEST_RIP, GuestRip + ExitInstructionLength);
        return TRUE;
    }

    default:

        // Unknown exit reason - let it pass
        return FALSE;
    }
}
