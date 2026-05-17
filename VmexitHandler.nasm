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
;   [rsp+24] = saved RAX   (guest RAX)
;   [rsp+16] = saved RBX
;   [rsp+ 8] = saved RCX   (guest RCX)
;   [rsp+ 0] = saved RDX   (guest RDX)
; ============================================================
MinimalVmexitHandlerStart:
    push rax
    push rbx
    push rcx
    push rdx

    ; ── Read exit reason (keep in RBX throughout) ─────────────
    mov  rcx, 0x4402
    vmread rbx, rcx
    and  rbx, 0xFFFF        ; basic exit reason in RBX

    ; ── Debug: log exit reason to ring buffer ─────────────────
    ; Step 1: get our runtime page base using LEA anchor.
    ;         page_base = &.anchor - (.anchor - MinimalVmexitHandlerStart) - 16
    lea  rax, [rel .anchor]   ; rax = runtime addr of .anchor
.anchor:
    sub  rax, (.anchor - MinimalVmexitHandlerStart) + 16
    ; rax = HandlerDest page base

    ; Step 2: atomically increment counter, bail if >= 64
    mov  rcx, [rax + 8]       ; current count
    cmp  rcx, 64
    jge  .log_done
    inc  qword [rax + 8]

    ; Step 3: write 2-byte exit reason at counter*4 offset in debug page
    mov  rdx, [rax]           ; debug log page address
    test rdx, rdx
    jz   .log_done
    shl  rcx, 2               ; byte offset = count * 4
    add  rdx, rcx
    mov  [rdx],   bl          ; exit reason low byte
    mov  [rdx+1], bh          ; exit reason high byte
    mov  word [rdx+2], 0      ; pad

    ; Step 4: Write directly to COM1 (0x3F8) so we can see it before freeze!
    ; We write "E:XX " where XX is the hex representation of the exit reason
    push rdx
    push rax
    push rbx
    push rcx
    
    mov  dx, 0x3F8
    mov  al, 'E'
    out  dx, al
    mov  al, ':'
    out  dx, al

    ; High nibble
    mov  al, bl
    shr  al, 4
    cmp  al, 10
    jl   .high_digit
    add  al, 'A' - 10
    jmp  .high_out
.high_digit:
    add  al, '0'
.high_out:
    out  dx, al

    ; Low nibble
    mov  al, bl
    and  al, 0x0F
    cmp  al, 10
    jl   .low_digit
    add  al, 'A' - 10
    jmp  .low_out
.low_digit:
    add  al, '0'
.low_out:
    out  dx, al

    mov  al, ' '
    out  dx, al

    pop  rcx
    pop  rbx
    pop  rax
    pop  rdx

.log_done:
    ; RBX still holds exit reason; RAX is scratch

    ; ── CPUID (0x0A = 10) ────────────────────────────────────
    cmp  rbx, 0x0A
    jne  .not_cpuid

    mov  rax, [rsp + 24]   ; guest RAX (leaf)
    mov  rcx, [rsp +  8]   ; guest RCX (subleaf)
    cmp  rax, 1
    jne  .cpuid_exec
    cpuid
    btr  ecx, 31            ; clear Hypervisor Present bit
    btr  ecx, 5             ; clear VMX bit
    jmp  .cpuid_store
.cpuid_exec:
    cpuid
.cpuid_store:
    mov  [rsp + 24], rax
    mov  [rsp + 16], rbx
    mov  [rsp +  8], rcx
    mov  [rsp      ], rdx
    jmp  .advance_rip

.not_cpuid:
    ; ── RDRAND (0x39) / RDSEED (0x3D) — set CF=1 ──────────
    cmp  rbx, 0x39
    je   .set_cf
    cmp  rbx, 0x3D
    je   .set_cf
    jmp  .not_rand

.set_cf:
    mov  rcx, 0x6820        ; GUEST_RFLAGS
    vmread rax, rcx
    or   rax, 1             ; CF = 1
    vmwrite rcx, rax
    jmp  .advance_rip

.not_rand:
    ; ── RDMSR (0x1F) ─────────────────────────────────────────
    cmp  rbx, 0x1F
    jne  .not_rdmsr
    mov  rcx, [rsp + 8]    ; MSR index from guest RCX
    
    cmp  rcx, 0x3A         ; MSR_IA32_FEATURE_CONTROL
    jne  .do_rdmsr
    xor  rax, rax          ; Fake response: VMX disabled
    xor  rdx, rdx
    jmp  .rdmsr_done

.do_rdmsr:
    rdmsr                   ; result in EDX:EAX
.rdmsr_done:
    mov  [rsp + 24], rax   ; guest RAX (low 32)
    mov  [rsp      ], rdx  ; guest RDX (high 32)
    jmp  .advance_rip

.not_rdmsr:
    ; ── WRMSR (0x20) ─────────────────────────────────────────
    cmp  rbx, 0x20
    jne  .not_wrmsr
    mov  rcx, [rsp +  8]   ; MSR index
    mov  rax, [rsp + 24]   ; low 32
    mov  rdx, [rsp      ]  ; high 32
    wrmsr
    jmp  .advance_rip

.not_wrmsr:
    ; ── XSETBV (0x37) ────────────────────────────────────────
    cmp  rbx, 0x37
    jne  .not_xsetbv
    mov  rcx, [rsp +  8]   ; XCR index
    mov  rax, [rsp + 24]   ; low 32 bits
    mov  rdx, [rsp      ]  ; high 32 bits
    xsetbv
    jmp  .advance_rip

.not_xsetbv:
    ; ── INVD (0x0D) ──────────────────────────────────────────
    cmp  rbx, 0x0D
    jne  .advance_rip
    wbinvd

    ; ── Advance guest RIP by VM_EXIT_INSTRUCTION_LEN ─────────
.advance_rip:
    mov  rcx, 0x440C        ; VM_EXIT_INSTRUCTION_LEN
    vmread rdx, rcx
    mov  rcx, 0x681E        ; GUEST_RIP
    vmread rax, rcx
    add  rax, rdx
    vmwrite rcx, rax

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

