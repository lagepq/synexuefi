bits 64
default rel

global AsmVmexitHandler
global AsmVmlaunch
global AsmVmlaunchAndCaptureState
global MinimalVmexitHandlerStart
global MinimalVmexitHandlerEnd
global __invept
extern VmexitDispatcher


section .text

; ============================================================
; AsmVmexitHandler - runs in VMX-root on every VM-Exit
; Used only if we're still in EFI environment.
; ============================================================
AsmVmexitHandler:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov  rcx, rsp          ; ptr to GUEST_REGISTERS (1st arg)
    sub  rsp, 0x28         ; shadow space
    call VmexitDispatcher
    add  rsp, 0x28

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    vmresume
    hlt
    jmp $

; ============================================================
; MinimalVmexitHandlerStart
; Position-independent VM-Exit stub copied to EfiRuntimeServicesData.
;
; The Vmcs.c places a 16-byte debug header BEFORE this code in the
; same page:
;   [page_base + 0]  QWORD  debug log page physical address
;   [page_base + 8]  QWORD  exit-reason counter
; HOST_RIP = page_base + 16  (i.e. the first byte of this code).
;
; We recover page_base at runtime using a LEA anchor trick so the
; code stays fully position-independent after CopyMem.
;
; Stack layout after the four pushes:
;   [rsp+48] = saved RAX   (guest RAX)
;   [rsp+40] = saved RBX
;   [rsp+32] = saved RCX   (guest RCX)
;   [rsp+24] = saved RDX   (guest RDX)
;   [rsp+16] = saved r13
;   [rsp+ 8] = saved r14
;   [rsp+ 0] = saved r15
; ============================================================
MinimalVmexitHandlerStart:
    push rax
    push rbx
    push rcx
    push rdx
    push r13
    push r14
    push r15

    ; ── Read exit reason ──────────────────────────────────────
    mov  rcx, 0x4402
    vmread rbx, rcx
    and  rbx, 0xFFFF

    ; ── Get GUEST_RIP for debugging ──────────────────────────
    push rbx
    mov  rcx, 0x681E        ; GUEST_RIP
    vmread rax, rcx
    mov  r15, rax           ; Save GUEST_RIP in R15 (non-volatile across our code)
    pop  rbx

    ; ── Debug ring buffer (first 64 exits) ───────────────────
    lea  rax, [rel .anchor]
.anchor:
    sub  rax, (.anchor - MinimalVmexitHandlerStart) + 16

    mov  rcx, [rax + 8]
    cmp  rcx, 64
    jge  .log_done
    inc  qword [rax + 8]

    mov  rdx, [rax]
    test rdx, rdx
    jz   .log_done
    shl  rcx, 2
    add  rdx, rcx
    mov  [rdx],   bl
    mov  [rdx+1], bh
    mov  word [rdx+2], 0

.log_done:
    ; ── ALWAYS log to COM1: "E:XX R:XXXXXXXXXXXXXXXX " ────────
    push rdx
    push rax
    push r15
    
    mov  dx, 0x3F8
    
    ; "E:"
    mov  al, 'E'
    out  dx, al
    mov  al, ':'
    out  dx, al
    
    ; Exit reason (2 hex digits)
    mov  al, bl
    shr  al, 4
    cmp  al, 10
    jl   .h1
    add  al, 'A' - 10
    jmp  .h2
.h1:
    add  al, '0'
.h2:
    out  dx, al
    
    mov  al, bl
    and  al, 0x0F
    cmp  al, 10
    jl   .l1
    add  al, 'A' - 10
    jmp  .l2
.l1:
    add  al, '0'
.l2:
    out  dx, al
    
    ; " R:"
    mov  al, ' '
    out  dx, al
    mov  al, 'R'
    out  dx, al
    mov  al, ':'
    out  dx, al
    
    ; GUEST_RIP (16 hex digits, high to low)
    mov  rcx, 16
    mov  r14, r15           ; R15 = GUEST_RIP
.rip_loop:
    dec  rcx
    mov  rax, r14
    mov  r13, rcx
    shl  r13, 2             ; r13 = rcx * 4
    
    ; Shift RAX by the value in R13
    ; Since shr only accepts CL or an immediate, we'll temporarily swap RCX and R13
    push rcx
    mov  rcx, r13
    shr  rax, cl
    pop  rcx
    
    and  al, 0x0F
    cmp  al, 10
    jl   .rip_digit
    add  al, 'A' - 10
    jmp  .rip_out
.rip_digit:
    add  al, '0'
.rip_out:
    out  dx, al
    test rcx, rcx
    jnz  .rip_loop
    
    ; " "
    mov  al, ' '
    out  dx, al
    
    pop  r15
    pop  rax
    pop  rdx

    ; ── CPUID (0x0A = 10) ────────────────────────────────────
    cmp  rbx, 0x0A
    jne  .not_cpuid

    mov  rax, [rsp + 48]
    mov  rcx, [rsp + 32]
    
    ; If EAX is in the hypervisor leaf range (0x40000000 - 0x400000FF), return all 0s
    cmp  rax, 0x40000000
    jb   .not_hv_leaf
    cmp  rax, 0x400000FF
    ja   .not_hv_leaf
    
    xor  rax, rax
    xor  rbx, rbx
    xor  rcx, rcx
    xor  rdx, rdx
    jmp  .cpuid_store

.not_hv_leaf:
    cmp  rax, 1
    jne  .cpuid_exec
    cpuid
    btr  ecx, 31            ; Hide hypervisor present bit
    btr  ecx, 5             ; Hide VMX support bit
    jmp  .cpuid_store
.cpuid_exec:
    cpuid
.cpuid_store:
    mov  [rsp + 48], rax
    mov  [rsp + 40], rbx
    mov  [rsp + 32], rcx
    mov  [rsp + 24], rdx
    jmp  .advance_rip

.not_cpuid:
    ; ── HLT (0x0C) ───────────────────────────────────────────
    cmp  rbx, 0x0C
    jne  .not_hlt
    ; Just advance RIP past HLT and resume (ignore the halt)
    jmp  .advance_rip

.not_hlt:
    ; ── RDRAND (0x39) / RDSEED (0x3D) ────────────────────────
    cmp  rbx, 0x39
    je   .set_cf
    cmp  rbx, 0x3D
    je   .set_cf
    jmp  .not_rand

.set_cf:
    mov  rcx, 0x6820
    vmread rax, rcx
    or   rax, 1
    vmwrite rcx, rax
    jmp  .advance_rip

.not_rand:
    ; ── RDMSR (0x1F) ─────────────────────────────────────────
    cmp  rbx, 0x1F
    jne  .not_rdmsr
    mov  rcx, [rsp + 32]
    
    cmp  rcx, 0x3A
    jne  .do_rdmsr
    xor  rax, rax
    xor  rdx, rdx
    jmp  .rdmsr_done

.do_rdmsr:
    rdmsr
.rdmsr_done:
    mov  [rsp + 48], rax
    mov  [rsp + 24], rdx
    jmp  .advance_rip

.not_rdmsr:
    ; ── WRMSR (0x20) ─────────────────────────────────────────
    cmp  rbx, 0x20
    jne  .not_wrmsr
    mov  rcx, [rsp + 32]   ; MSR index
    mov  rax, [rsp + 48]   ; low 32
    mov  rdx, [rsp + 24]   ; high 32
    wrmsr
    jmp  .advance_rip

.not_wrmsr:
    ; ── XSETBV (0x37) ────────────────────────────────────────
    cmp  rbx, 0x37
    jne  .not_xsetbv
    mov  rcx, [rsp + 32]   ; XCR index
    mov  rax, [rsp + 48]   ; low 32 bits
    mov  rdx, [rsp + 24]   ; high 32 bits
    xsetbv
    jmp  .advance_rip

.not_xsetbv:
    ; ── INVD (0x0D) ──────────────────────────────────────────
    cmp  rbx, 0x0D
    jne  .not_invd
    wbinvd
    jmp  .advance_rip

.not_invd:
    ; ── IO instruction (0x1E) ────────────────────────────────
    cmp  rbx, 0x1E
    jne  .not_io
    ; Just pass through for now (emulation is complex)
    jmp  .advance_rip

.not_io:
    ; ── Unknown: HALT the system with debug output ───────────
    ; Write "X:XX " to COM to indicate unknown exit
    push rdx
    push rax
    mov  dx, 0x3F8
    mov  al, 'X'
    out  dx, al
    mov  al, ':'
    out  dx, al
    mov  al, bl
    shr  al, 4
    cmp  al, 10
    jl   .x1
    add  al, 'A' - 10
    jmp  .x2
.x1:
    add  al, '0'
.x2:
    out  dx, al
    mov  al, bl
    and  al, 0x0F
    cmp  al, 10
    jl   .x3
    add  al, 'A' - 10
    jmp  .x4
.x3:
    add  al, '0'
.x4:
    out  dx, al
    mov  al, ' '
    out  dx, al
    pop  rax
    pop  rdx
    
    ; HALT — unknown exit, can't continue safely
    cli
    hlt
    jmp $ - 1

.advance_rip:
    mov  rcx, 0x440C
    vmread rdx, rcx
    mov  rcx, 0x681E
    vmread rax, rcx
    add  rax, rdx
    vmwrite rcx, rax

.resume_only:
    pop  r15
    pop  r14
    pop  r13
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    vmresume
    hlt
    jmp  $ - 2
MinimalVmexitHandlerEnd:



; ============================================================
; AsmVmlaunch - simple VMLAUNCH, returns 0 on failure
; ============================================================
AsmVmlaunch:
    vmlaunch
    xor eax, eax
    ret

; ============================================================
; AsmVmlaunchAndCaptureState
;
; Captures current RIP and RSP into VMCS as Guest state,
; then executes VMLAUNCH.
;
; After VMLAUNCH: guest resumes at caller's return site.
; If VMLAUNCH fails: execution falls through (caller continues).
; ============================================================
AsmVmlaunchAndCaptureState:
    ; [rsp+0] = return address (becomes Guest RIP)
    mov  rax, [rsp]        ; Guest RIP = return address
    lea  rdx, [rsp+8]      ; Guest RSP = RSP after ret consumed

    ; VMWRITE GUEST_RIP (0x681E)
    mov  rcx, 0x681E
    vmwrite rcx, rax

    ; VMWRITE GUEST_RSP (0x681C)
    mov  rcx, 0x681C
    vmwrite rcx, rdx

    ; Launch
    vmlaunch
    
    ; If we get here, VMLAUNCH failed!
    ; Read the error code from VM_INSTRUCTION_ERROR (0x4400) if possible, or just return 0.
    xor rax, rax
    ret

; ============================================================
; AsmVmxon - robust assembly wrapper for Intel VMXON
; RCX = VMXON region physical address (64-bit)
; Returns: 0 on success, 1 on fail (CF=1), 2 on fail (ZF=1)
; ============================================================
global AsmVmxon
AsmVmxon:
    push rcx               ; Store physical address in memory on stack
    vmxon [rsp]            ; Execute vmxon with memory operand
    pop rcx                ; Restore stack pointer
    jc .fail_carry
    jz .fail_zero
    xor al, al             ; Return 0 on success
    ret

.fail_carry:
    mov al, 1              ; Return 1 on Carry Flag
    ret

.fail_zero:
    mov al, 2              ; Return 2 on Zero Flag
    ret

; ============================================================
; __invept - dynamic EPT TLB invalidation wrapper for compiler
; RCX = Type, RDX = Pointer to 128-bit descriptor
; ============================================================
__invept:
    invept rcx, [rdx]
    ret