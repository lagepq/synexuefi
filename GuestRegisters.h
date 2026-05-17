#ifndef GUEST_REGISTERS_H
#define GUEST_REGISTERS_H

#include <Uefi.h>

#pragma pack(push, 1)
typedef struct _GUEST_REGISTERS {
    UINT64 R15;
    UINT64 R14;
    UINT64 R13;
    UINT64 R12;
    UINT64 R11;
    UINT64 R10;
    UINT64 R9;
    UINT64 R8;
    UINT64 Rbp;
    UINT64 Rdi;
    UINT64 Rsi;
    UINT64 Rdx;
    UINT64 Rcx;
    UINT64 Rbx;
    UINT64 Rax;
} GUEST_REGISTERS, *PGUEST_REGISTERS;
#pragma pack(pop)

// VM-Exit reasons we care about
#define EXIT_REASON_CPUID           10
#define EXIT_REASON_VMCALL          18
#define EXIT_REASON_MSR_READ        31
#define EXIT_REASON_MSR_WRITE       32
#define EXIT_REASON_EPT_VIOLATION   48


// CPUID related
#define CPUID_VERSION_INFO_LEAF     0x00000001
#define CPUID_HV_VENDOR_LEAF        0x40000000

// VMCS fields
#define GUEST_RIP                   0x0000681E
#define GUEST_RSP                   0x0000681C
#define GUEST_RFLAGS                0x00006820
#define VM_EXIT_REASON              0x00004402
#define VM_EXIT_INSTRUCTION_LEN     0x0000440C
#define GUEST_CS_SELECTOR           0x00000802

// Backdoor related
#define SYNEX_BACKDOOR_LEAF         0x1337BEEF
#define SYNEX_BACKDOOR_MAGIC        0x53594E58 // "SYNX"


// Host state
#define HOST_CR0                    0x00006C00
#define HOST_CR3                    0x00006C02
#define HOST_CR4                    0x00006C04
#define HOST_RSP                    0x00006C14
#define HOST_RIP                    0x00006C16

#endif // GUEST_REGISTERS_H
