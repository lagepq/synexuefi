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
; The Vmcs.c places a 64-byte state header BEFORE this code:
;   [base + 0]   QWORD  debug log page physical address
;   [base + 8]   QWORD  exit-reason counter
;   [base + 16]  QWORD  virtual IA32_DEBUGCTL
;   [base + 24]  QWORD  virtual IA32_RTIT_CTL
;   [base + 32]  QWORD  captured LSTAR (kernel base)
;   [base + 40]  QWORD  authenticated CR3
;   [base + 48]  QWORD  backdoor flags (bit 0 = enabled)
;   [base + 56]  QWORD  reserved
; HOST_RIP = base + 64  (first byte of this code).
;
; Stack layout after pushes:
;   [rsp+48] = saved RAX   (guest RAX)
;   [rsp+40] = saved RBX   (guest RBX)
;   [rsp+32] = saved RCX   (guest RCX)
;   [rsp+24] = saved RDX   (guest RDX)
;   [rsp+16] = saved R13
;   [rsp+ 8] = saved R14
;   [rsp+ 0] = saved R15
; ============================================================

; Header field offsets
%define HDR_DEBUGLOG    0
%define HDR_EXITCOUNT   8
%define HDR_VDEBUGCTL   16
%define HDR_VRTITCTL    24
%define HDR_LSTAR       32
%define HDR_AUTH_CR3    40
%define HDR_BD_FLAGS    48
%define HDR_SIZE        128
%define HDR_FB_BASE     64
%define HDR_FB_WIDTH    72
%define HDR_FB_HEIGHT   76
%define HDR_FB_PPS      80
%define HDR_FB_CURX     84
%define HDR_FB_CURY     88
%define HDR_LAST_EXIT   104    ; last exit reason saved for vmresume-fail diagnosis

; Guest register offsets on stack (after 7 pushes)
%define STK_RAX  48
%define STK_RBX  40
%define STK_RCX  32
%define STK_RDX  24
%define STK_R13  16
%define STK_R14   8
%define STK_R15   0

; Exit reasons
%define EXIT_CPUID   0x0A
%define EXIT_HLT     0x0C
%define EXIT_INVD    0x0D
%define EXIT_VMCALL  0x12
%define EXIT_CR      0x1C
%define EXIT_IO      0x1E
%define EXIT_RDMSR   0x1F
%define EXIT_WRMSR   0x20
%define EXIT_EPT     0x30
%define EXIT_INVEPT  0x32
%define EXIT_RDRAND  0x39
%define EXIT_XSETBV  0x37
%define EXIT_RDSEED  0x3D
%define EXIT_PAUSE   0x26
; Additional exit reasons
%define EXIT_INT_WIN 0x07    ; Interrupt-window exit
%define EXIT_NMI_WIN 0x08    ; NMI-window exit
%define EXIT_VMXOFF  0x1A    ; Guest VMXOFF instruction
%define EXIT_DR      0x1D    ; Debug register access
%define EXIT_INVLPG  0x0E    ; INVLPG instruction
%define EXIT_RDTSC   0x10    ; RDTSC (if exiting enabled)
%define EXIT_MONITOR 0x27    ; MONITOR instruction
%define EXIT_MWAIT   0x24    ; MWAIT instruction
%define EXIT_SIPI    0x04    ; Startup IPI
%define EXIT_INIT    0x03    ; INIT signal
%define EXIT_TRIPL   0x02    ; Triple fault
%define EXIT_ENTRY_FAIL 0x21 ; VM-entry failure due to invalid guest state

; Backdoor constants
%define SYNEX_LEAF   0x1337BEEF
%define SYNEX_MAGIC  0x53594E58

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

    ; ── Get header base address (position-independent) ────────
    lea  rax, [rel .anchor]
.anchor:
    sub  rax, (.anchor - MinimalVmexitHandlerStart) + HDR_SIZE
    mov  r14, rax              ; r14 = header base (kept throughout)
    mov  [r14 + HDR_LAST_EXIT], rbx   ; save for vmresume-fail indicator

    ; ── Real-Time Screen Timeline Logging ─────────────────────
    ; Draw a super-fast 4x4 mini colored pixel block representing the exit reason.
    ; This works on real PCs without COM ports, has zero overhead, and shows exactly
    ; what exits ran immediately before a crash/hang.
    call VgaDrawMiniPixel

    ; ── Screen Logging (VGA Draw) ─────────────────────────────
    ; Draw colored square based on exit reason
    mov  eax, 0x00FFFFFF       ; White
    cmp  rbx, 0
    jne  .c1
    mov  eax, 0x00FF0000       ; Red = Exception
    jmp  .draw
.c1:
    cmp  rbx, EXIT_EPT
    jne  .c2
    mov  eax, 0x000000FF       ; Blue = EPT
    jmp  .draw
.c2:
    cmp  rbx, EXIT_XSETBV
    jne  .c3
    mov  eax, 0x0000FFFF       ; Cyan = XSETBV
    jmp  .draw
.c3:
    cmp  rbx, EXIT_CPUID
    jne  .c4
    mov  eax, 0x0000FF00       ; Green = CPUID
    jmp  .draw
.c4:
    mov  eax, 0x00FFFF00       ; Yellow = Other

.draw:
    ; Draw visual indicator (for now just draw '0' or colored character)
    ; Al = character index, eax = color
    mov al, bl                 ; use exit reason low byte as index
    cmp al, 20                 ; limit to our font data bounds
    jb .char_ok
    mov al, 20                 ; space/fallback character
.char_ok:

    ; Exit reasons are tracked in the debug ring buffer below.

    ; ── Debug ring buffer (first 64 exits) ────────────────────
    mov  rcx, [r14 + HDR_EXITCOUNT]
    cmp  rcx, 64
    jge  .log_done

    ; Only draw the first 64 exits to prevent massive slowdown during bootmgr CPUID calibration loops
    ; call VgaDrawChar

    mov  rdx, [r14 + HDR_DEBUGLOG]
    test rdx, rdx
    jz   .log_done
    shl  rcx, 2
    add  rdx, rcx
    mov  [rdx],   bl
    mov  [rdx+1], bh
    mov  word [rdx+2], 0

.log_done:

    ; ─────────────────────────────────────────────────────────
    ; EXIT REASON DISPATCH
    ; ─────────────────────────────────────────────────────────

    ; ── Exception/NMI (0x00): Re-inject to guest ─────────────────────
    cmp  rbx, 0
    jne  .not_exception

    ; Read VM_EXIT_INTR_INFO (0x4404) to get what exception was delivered
    mov  rcx, 0x4404
    vmread rax, rcx         ; rax = exit interruption information

    ; Re-inject the exception into the guest via VM_ENTRY_INTR_INFO_FIELD (0x4016)
    ; Keep the VALID bit (31) + type (bits 10:8) + vector (bits 7:0)
    and  eax, 0x800007FF    ; Keep bit31 (valid), bits10:8 (type), bits7:0 (vector)

    ; Check if VM_EXIT_INTR_ERROR_CODE_VALID (bit 11) was set in the exit info
    mov  rcx, 0x4404
    vmread rdx, rcx
    test rdx, (1 << 11)
    jz   .exc_no_errcode

    ; If error code valid: copy error code from VM_EXIT_INTR_ERROR_CODE (0x4406)
    or   rax, (1 << 11)           ; Set error-code-valid bit in entry info
    mov  rcx, 0x4016
    vmwrite rcx, rax
    mov  rcx, 0x4406
    vmread rdx, rcx
    mov  rcx, 0x4018              ; VM_ENTRY_EXCEPTION_ERROR_CODE
    vmwrite rcx, rdx
    jmp  .exc_done_inject

.exc_no_errcode:
    mov  rcx, 0x4016
    vmwrite rcx, rax

.exc_done_inject:
    ; Do NOT advance RIP - exception re-delivery means guest re-executes same instruction
    jmp  .resume_only

.not_exception:

    ; ── CPUID (0x0A) ─────────────────────────────────────────
    cmp  rbx, EXIT_CPUID
    jne  .not_cpuid

    mov  rax, [rsp + STK_RAX]   ; guest EAX (leaf)
    mov  rcx, [rsp + STK_RCX]   ; guest ECX (subleaf)

    ; --- SYNEX BACKDOOR CHECK ---
    cmp  eax, SYNEX_LEAF
    jne  .no_backdoor
    cmp  dword [rsp + STK_RDX], SYNEX_MAGIC
    jne  .no_backdoor

    ; Check CPL == 3 from guest CS selector
    push rax
    mov  rcx, 0x0802            ; GUEST_CS_SELECTOR
    vmread rax, rcx
    and  eax, 3
    cmp  eax, 3
    pop  rax
    jne  .backdoor_reject

    ; Read guest CR3 for authentication
    push rax
    mov  rcx, 0x6802            ; GUEST_CR3
    vmread r13, rcx
    pop  rax

    mov  ecx, [rsp + STK_RCX]  ; subleaf = operation

    ; ── Op 0: Init / Authenticate ──
    cmp  ecx, 0
    jne  .bd_op1
    ; Check if already locked to another CR3
    mov  rax, [r14 + HDR_AUTH_CR3]
    test rax, rax
    jz   .bd_op0_fresh
    cmp  rax, r13
    je   .bd_op0_ok
    ; Locked to different process — R11 = 1
    jmp  .bd_op0_locked

.bd_op0_fresh:
    mov  [r14 + HDR_AUTH_CR3], r13
    or   qword [r14 + HDR_BD_FLAGS], 1
.bd_op0_ok:
    mov  qword [rsp + STK_RAX], 0    ; result code 0 = success
    jmp  .bd_done_success

.bd_op0_locked:
    mov  qword [rsp + STK_RAX], 1
    jmp  .bd_done_fail

    ; ── Op 1: TranslateVirtual ──
.bd_op1:
    cmp  ecx, 1
    jne  .bd_op2

    ; Check authentication
    mov  rax, [r14 + HDR_BD_FLAGS]
    test rax, 1
    jz   .bd_not_auth
    mov  rax, [r14 + HDR_AUTH_CR3]
    cmp  rax, r13
    jne  .bd_not_auth

    ; R8 = guest virtual address to translate
    ; R8 is not on our stack, we need it from the actual register
    ; After pushes, R8 is still in R8 (only rax,rbx,rcx,rdx,r13,r14,r15 pushed)
    ; Wait - r13, r14, r15 are pushed, so r8 is untouched. Good.
    mov  rax, r8                ; GVA to translate
    mov  rcx, [r14 + HDR_AUTH_CR3] ; CR3 for page walk

    ; --- Inline 4-level page table walk ---
    ; rcx = CR3, rax = GVA
    mov  r15, rax               ; save GVA in r15
    mov  r13, 0x000FFFFFFFFFF000 ; PhysMask

    ; PML4
    and  rcx, r13               ; CR3 & mask = PML4 base
    mov  rdx, r15
    shr  rdx, 39
    and  edx, 0x1FF             ; PML4 index
    mov  rax, [rcx + rdx*8]     ; PML4E
    test al, 1
    jz   .translate_fail

    ; PDPT
    and  rax, r13
    mov  rdx, r15
    shr  rdx, 30
    and  edx, 0x1FF
    mov  rax, [rax + rdx*8]     ; PDPTE
    test al, 1
    jz   .translate_fail
    test al, 0x80               ; 1GB huge page?
    jnz  .translate_1gb

    ; PD
    and  rax, r13
    mov  rdx, r15
    shr  rdx, 21
    and  edx, 0x1FF
    mov  rax, [rax + rdx*8]     ; PDE
    test al, 1
    jz   .translate_fail
    test al, 0x80               ; 2MB large page?
    jnz  .translate_2mb

    ; PT
    and  rax, r13
    mov  rdx, r15
    shr  rdx, 12
    and  edx, 0x1FF
    mov  rax, [rax + rdx*8]     ; PTE
    test al, 1
    jz   .translate_fail

    ; 4KB page
    and  rax, r13
    mov  rdx, r15
    and  edx, 0xFFF
    or   rax, rdx               ; GPA = (PTE & mask) | offset
    jmp  .translate_ok

.translate_1gb:
    mov  r13, 0x000FFFFFC0000000 ; 1GB-aligned phys mask
    and  rax, r13
    mov  rdx, r15
    and  edx, 0x3FFFFFFF        ; offset within 1GB
    or   rax, rdx
    jmp  .translate_ok

.translate_2mb:
    mov  r13, 0x000FFFFFFFE00000 ; 2MB-aligned phys mask
    and  rax, r13
    mov  rdx, r15
    and  edx, 0x1FFFFF          ; offset within 2MB
    or   rax, rdx
    jmp  .translate_ok

.translate_fail:
    ; R10 = 0, R11 = 3 (translation fail)
    xor  r10, r10
    mov  qword [rsp + STK_RAX], 3
    jmp  .advance_rip

.translate_ok:
    mov  r10, rax               ; R10 = GPA result
    mov  qword [rsp + STK_RAX], 0 ; R11=0 via RAX (we'll fix below)
    jmp  .bd_done_success

    ; ── Op 2: ReadPhysical (16 bytes at GPA in R8) ──
.bd_op2:
    cmp  ecx, 2
    jne  .bd_op5

    mov  rax, [r14 + HDR_BD_FLAGS]
    test rax, 1
    jz   .bd_not_auth
    mov  rax, [r14 + HDR_AUTH_CR3]
    cmp  rax, r13
    jne  .bd_not_auth

    ; R8 = GPA to read
    test r8, r8
    jz   .bd_read_fail
    mov  r10, [r8]              ; first 8 bytes
    mov  r9,  [r8 + 8]         ; second 8 bytes
    mov  qword [rsp + STK_RAX], 0
    jmp  .bd_done_success

.bd_read_fail:
    mov  qword [rsp + STK_RAX], 4
    jmp  .bd_done_fail

    ; ── Op 5: GetLogBuffer ──
.bd_op5:
    cmp  ecx, 5
    jne  .bd_op10

    mov  rax, [r14 + HDR_BD_FLAGS]
    test rax, 1
    jz   .bd_not_auth

    ; Return log buffer address from header
    ; (Uses the memory log buffer, not the debug ring buffer)
    ; For now, return 0 as placeholder - ComLogger buffer addr not stored in header
    xor  r10, r10
    mov  qword [rsp + STK_RAX], 0
    jmp  .bd_done_success

    ; ── Op 10: GetKernelBase ──
.bd_op10:
    cmp  ecx, 10
    jne  .bd_unknown

    mov  rax, [r14 + HDR_BD_FLAGS]
    test rax, 1
    jz   .bd_not_auth

    mov  r10, [r14 + HDR_LSTAR]
    mov  qword [rsp + STK_RAX], 0
    jmp  .bd_done_success

.bd_unknown:
    mov  qword [rsp + STK_RAX], 0xFF
    jmp  .bd_done_fail

.bd_not_auth:
    mov  qword [rsp + STK_RAX], 2
    jmp  .bd_done_fail

.backdoor_reject:
    ; Non-ring-3 caller - fall through to normal CPUID
    mov  rax, [rsp + STK_RAX]
    mov  rcx, [rsp + STK_RCX]
    jmp  .no_backdoor

.bd_done_success:
    ; For backdoor: guest expects results in R10, R11
    ; R10 is still a live register (not saved to stack), so it's already set
    ; R11 is also a live register. We stored result code in [rsp+STK_RAX]
    ; but we actually need it in R11. So move it now:
    mov  r11, [rsp + STK_RAX]
    mov  qword [rsp + STK_RAX], 0  ; Restore RAX to something benign
    jmp  near .advance_rip

.bd_done_fail:
    mov  r11, [rsp + STK_RAX]
    mov  qword [rsp + STK_RAX], 0
    jmp  near .advance_rip

    ; --- NORMAL CPUID PATH ---
.no_backdoor:
    ; Hypervisor leaves -> zero out
    cmp  eax, 0x40000000
    jb   .not_hv_leaf
    cmp  eax, 0x400000FF
    ja   .not_hv_leaf
    xor  eax, eax
    xor  ebx, ebx
    xor  ecx, ecx
    xor  edx, edx
    jmp  .cpuid_store

.not_hv_leaf:
    cmp  eax, 1
    je   .cpuid_leaf1
    cmp  eax, 7
    je   .cpuid_leaf7
    cmp  eax, 0xD
    je   .cpuid_leafD
    jmp  .cpuid_exec

.cpuid_leaf1:
    cpuid
    btr  ecx, 31            ; Clear HypervisorPresent
    ; Keep VMX support enabled to allow Windows 11 VBS/Hyper-V to boot successfully
    jmp  .cpuid_store

.cpuid_leaf7:
    ; Leaf 7 uses ECX as subleaf. We mask subleaf 0 and subleaf 1.
    cmp  ecx, 0
    je   .cpuid_leaf7_sub0
    cmp  ecx, 1
    je   .cpuid_leaf7_sub1
    jmp  .cpuid_exec

.cpuid_leaf7_sub0:
    cpuid
    btr  ecx, 7             ; Mask CET_SS (User Shadow Stacks)
    btr  edx, 20            ; Mask CET_IBT (Indirect Branch Tracking)
    jmp  .cpuid_store

.cpuid_leaf7_sub1:
    cpuid
    btr  eax, 18            ; Mask CET_SSS (Supervisor Shadow Stacks)
    jmp  .cpuid_store

.cpuid_leafD:
    ; Leaf 13 (0xD) uses ECX as subleaf. We check the input ECX from stack.
    mov  ecx, [rsp + STK_RCX]
    mov  r8d, ecx           ; Save subleaf before cpuid overwrites it
    cpuid
    cmp  r8d, 0
    je   .cpuid_leafD_sub0
    cmp  r8d, 1
    je   .cpuid_leafD_sub1
    jmp  .cpuid_store

.cpuid_leafD_sub0:
    ; Mask out CET_U (bit 11) and CET_S (bit 12) from returned EAX
    and  eax, 0xFFFFE7FF
    jmp  .cpuid_store

.cpuid_leafD_sub1:
    ; Mask out CET_U (bit 11) and CET_S (bit 12) from returned ECX
    and  ecx, 0xFFFFE7FF
    jmp  .cpuid_store

.cpuid_exec:
    cpuid

.cpuid_store:
    mov  [rsp + STK_RAX], rax
    mov  [rsp + STK_RBX], rbx
    mov  [rsp + STK_RCX], rcx
    mov  [rsp + STK_RDX], rdx
    jmp  .advance_rip

.not_cpuid:
    ; ── HLT (0x0C) ───────────────────────────────────────────
    cmp  rbx, EXIT_HLT
    jne  .not_hlt
    jmp  .advance_rip

.not_hlt:
    ; ── RDRAND (0x39) / RDSEED (0x3D) ────────────────────────
    cmp  rbx, EXIT_RDRAND
    je   .handle_rand
    cmp  rbx, EXIT_RDSEED
    jne  .not_rand
.handle_rand:
    ; Set CF=1 in guest RFLAGS to indicate success
    mov  rcx, 0x6820
    vmread rax, rcx
    or   rax, 1
    vmwrite rcx, rax
    jmp  .advance_rip

.not_rand:
    ; ── RDMSR (0x1F) ─────────────────────────────────────────
    cmp  rbx, EXIT_RDMSR
    jne  .not_rdmsr
    mov  rcx, [rsp + STK_RCX]  ; guest ECX = MSR number

    ; IA32_DEBUGCTL (0x1D9) - return virtual value
    cmp  ecx, 0x1D9
    jne  .rdmsr_not_debugctl
    mov  rax, [r14 + HDR_VDEBUGCTL]
    mov  rdx, rax
    shr  rdx, 32
    and  eax, 0xFFFFFFFF
    jmp  .rdmsr_finish

.rdmsr_not_debugctl:
    ; IA32_RTIT_CTL (0x570) - return virtual value
    cmp  ecx, 0x570
    jne  .rdmsr_not_rtit
    mov  rax, [r14 + HDR_VRTITCTL]
    mov  rdx, rax
    shr  rdx, 32
    and  eax, 0xFFFFFFFF
    jmp  .rdmsr_finish

.rdmsr_not_rtit:
    ; IA32_FEATURE_CONTROL (0x3A) - report locked and enabled VMX (value 5)
    cmp  ecx, 0x3A
    jne  .rdmsr_real
    mov  eax, 5
    xor  edx, edx
    jmp  .rdmsr_finish

.rdmsr_real:
    rdmsr

.rdmsr_finish:
    mov  [rsp + STK_RAX], rax
    mov  [rsp + STK_RDX], rdx
    jmp  .advance_rip

.not_rdmsr:
    ; ── WRMSR (0x20) ─────────────────────────────────────────
    cmp  rbx, EXIT_WRMSR
    jne  .not_wrmsr
    mov  rcx, [rsp + STK_RCX]  ; MSR number
    mov  rax, [rsp + STK_RAX]  ; low 32
    mov  rdx, [rsp + STK_RDX]  ; high 32

    ; IA32_DEBUGCTL (0x1D9) - sabotage LBR and BTS
    cmp  ecx, 0x1D9
    jne  .wrmsr_not_debugctl
    ; Compose full 64-bit value: (EDX << 32) | EAX
    mov  r15d, edx             ; zero-extend low 32 bits
    shl  r15, 32
    mov  r13d, eax             ; zero-extend low 32 bits
    or   r15, r13
    mov  [r14 + HDR_VDEBUGCTL], r15
    ; Sabotage: clear LBR (bit 0) and BTS (bit 7)
    and  eax, 0xFFFFFF7E       ; ~0x81
    wrmsr
    jmp  .advance_rip

.wrmsr_not_debugctl:
    ; IA32_RTIT_CTL (0x570) - sabotage TraceEn
    cmp  ecx, 0x570
    jne  .wrmsr_not_rtit
    mov  r15d, edx
    shl  r15, 32
    mov  r13d, eax
    or   r15, r13
    mov  [r14 + HDR_VRTITCTL], r15
    ; Sabotage: clear TraceEn (bit 0)
    and  eax, 0xFFFFFFFE
    wrmsr
    jmp  .advance_rip

.wrmsr_not_rtit:
    ; IA32_LSTAR (0xC0000082) - capture kernel syscall entry
    cmp  ecx, 0xC0000082
    jne  .wrmsr_real
    mov  r15d, edx
    shl  r15, 32
    mov  r13d, eax
    or   r15, r13
    mov  [r14 + HDR_LSTAR], r15
    ; Pass through the real write
.wrmsr_real:
    wrmsr
    jmp  .advance_rip

.not_wrmsr:
    ; ── XSETBV (0x37) ────────────────────────────────────────
    cmp  rbx, EXIT_XSETBV
    jne  .not_xsetbv
    mov  rcx, [rsp + STK_RCX]
    mov  rax, [rsp + STK_RAX]
    mov  rdx, [rsp + STK_RDX]
    xsetbv
    jmp  .advance_rip

.not_xsetbv:
    ; ── INVD (0x0D) ──────────────────────────────────────────
    cmp  rbx, EXIT_INVD
    jne  .not_invd
    wbinvd
    jmp  .advance_rip

.not_invd:
    ; ── PAUSE instruction (0x26) ─────────────────────────────
    cmp  rbx, EXIT_PAUSE
    jne  .not_pause
    pause
    jmp  .advance_rip

.not_pause:
    ; ── IO instruction (0x1E) ────────────────────────────────
    cmp  rbx, EXIT_IO
    jne  .not_io
    jmp  .advance_rip

.not_io:
    ; ── VMCALL (0x12) ────────────────────────────────────────
    cmp  rbx, EXIT_VMCALL
    jne  .not_vmcall
    mov  qword [rsp + STK_RAX], 0   ; RAX = 0
    jmp  .advance_rip

.not_vmcall:
    ; ── EPT Violation (0x30) ─────────────────────────────────
    ; With MTRR-aware EPT this should never fire. If it does, fall through
    ; to the unknown-exit halt (red EXIT:0x30 on screen) so we can diagnose it.
    cmp  rbx, EXIT_EPT
    jne  .not_ept
    jmp  .not_cr   ; fall through to unknown-exit halt

.not_ept:
    ; ── INVEPT (0x32 / 50) ───────────────────────────────────
    cmp  rbx, EXIT_INVEPT
    jne  .not_invept
    sub  rsp, 16
    mov  qword [rsp], 0
    mov  qword [rsp + 8], 0
    mov  rax, 2                 ; Type 2 = Global EPT invalidation
    invept rax, [rsp]
    add  rsp, 16
    jmp  .advance_rip

.not_invept:
    ; ── CR Access (0x1C) ─────────────────────────────────────
    cmp  rbx, EXIT_CR
    jne  .not_cr

    ; 1. Read Exit Qualification (contains details about the CR access)
    mov rcx, 0x6400            ; EXIT_QUALIFICATION
    vmread r13, rcx            ; r13 = Exit Qualification

    ; 2. Determine GPR index (bits 11:8 of qualification)
    mov rdx, r13
    shr rdx, 8
    and rdx, 0xF               ; rdx = GPR index

    ; Resolve guest register value into R8
    cmp rdx, 0
    je .gpr_rax
    cmp rdx, 1
    je .gpr_rcx
    cmp rdx, 2
    je .gpr_rdx
    cmp rdx, 3
    je .gpr_rbx
    cmp rdx, 4
    je .gpr_rsp
    cmp rdx, 5
    je .gpr_rbp
    cmp rdx, 6
    je .gpr_rsi
    cmp rdx, 7
    je .gpr_rdi
    cmp rdx, 8
    je .gpr_r8                 ; R8 already has guest R8
    cmp rdx, 9
    je .gpr_r9
    cmp rdx, 10
    je .gpr_r10
    cmp rdx, 11
    je .gpr_r11
    cmp rdx, 12
    je .gpr_r12
    cmp rdx, 13
    je .gpr_r13
    cmp rdx, 14
    je .gpr_r14
    cmp rdx, 15
    je .gpr_r15
    jmp .gpr_done

.gpr_rax:
    mov r8, [rsp + STK_RAX]
    jmp .gpr_done
.gpr_rcx:
    mov r8, [rsp + STK_RCX]
    jmp .gpr_done
.gpr_rdx:
    mov r8, [rsp + STK_RDX]
    jmp .gpr_done
.gpr_rbx:
    mov r8, [rsp + STK_RBX]
    jmp .gpr_done
.gpr_rsp:
    mov rcx, 0x681C            ; GUEST_RSP
    vmread r8, rcx
    jmp .gpr_done
.gpr_rbp:
    mov r8, rbp
    jmp .gpr_done
.gpr_rsi:
    mov r8, rsi
    jmp .gpr_done
.gpr_rdi:
    mov r8, rdi
    jmp .gpr_done
.gpr_r8:
    ; physical R8 is guest R8, do nothing
    jmp .gpr_done
.gpr_r9:
    mov r8, r9
    jmp .gpr_done
.gpr_r10:
    mov r8, r10
    jmp .gpr_done
.gpr_r11:
    mov r8, r11
    jmp .gpr_done
.gpr_r12:
    mov r8, r12
    jmp .gpr_done
.gpr_r13:
    mov r8, [rsp + STK_R13]
    jmp .gpr_done
.gpr_r14:
    mov r8, [rsp + STK_R14]
    jmp .gpr_done
.gpr_r15:
    mov r8, [rsp + STK_R15]
    jmp .gpr_done

.gpr_done:
    ; 3. Access Type (bits 5:4 of qualification)
    ; 0 = Move to CR, 1 = Move from CR
    mov rax, r13
    shr rax, 4
    and rax, 3                 ; rax = Access Type
    cmp rax, 0
    jne .cr_unhandled          ; Ignore CR reads (handled by CPU read shadow)

    ; 4. CR Number (bits 3:0 of qualification)
    mov rax, r13
    and rax, 0xF               ; rax = CR Number
    cmp rax, 0
    je .write_cr0
    cmp rax, 4
    je .write_cr4

.cr_unhandled:
    jmp .advance_rip

.write_cr0:
    mov rcx, 0x6800            ; GUEST_CR0
    vmwrite rcx, r8
    mov rcx, 0x6004            ; CR0_READ_SHADOW
    vmwrite rcx, r8
    jmp .advance_rip

.write_cr4:
    ; Write R8 | 0x2000 to GUEST_CR4 to keep VMX enabled physically
    mov rax, r8
    or  rax, 0x2000            ; Force VMXE bit physically
    mov rcx, 0x6804            ; GUEST_CR4
    vmwrite rcx, rax

    ; Write R8 & ~0x2000 to CR4_READ_SHADOW to hide VMX from guest reads
    mov rax, r8
    and rax, ~0x2000           ; Hide VMXE
    mov rcx, 0x6006            ; CR4_READ_SHADOW
    vmwrite rcx, rax

    jmp .advance_rip

.not_cr:
    ; ── External interrupt (0x01) ────────────────────────────────
    ; Interrupt was already acknowledged by CPU on VM-exit if
    ; "Acknowledge interrupt on exit" is NOT set in ExitCtls.
    ; Do NOT advance RIP - just resume and the interrupt will
    ; be delivered to the guest normally.
    cmp  rbx, 1
    jne  .not_ext_int
    jmp  .resume_only

.not_ext_int:
    ; ── Interrupt-window exit (0x07) ─────────────────────────
    cmp  rbx, EXIT_INT_WIN
    jne  .not_int_win
    jmp  .resume_only          ; do NOT advance RIP for window exits

.not_int_win:
    ; ── NMI-window exit (0x08) ───────────────────────────────
    cmp  rbx, EXIT_NMI_WIN
    jne  .not_nmi_win
    jmp  .resume_only          ; do NOT advance RIP for window exits

.not_nmi_win:
    ; ── INVLPG (0x0E) ────────────────────────────────────────
    cmp  rbx, EXIT_INVLPG
    jne  .not_invlpg
    jmp  .advance_rip

.not_invlpg:
    ; ── RDTSC (0x10) ─────────────────────────────────────────
    cmp  rbx, EXIT_RDTSC
    jne  .not_rdtsc
    rdtsc
    mov  [rsp + STK_RAX], rax
    mov  [rsp + STK_RDX], rdx
    jmp  .advance_rip

.not_rdtsc:
    ; ── VMXOFF (0x1A) ────────────────────────────────────────
    ; Guest is trying to turn off VMX; silently ignore to keep hypervisor alive.
    cmp  rbx, EXIT_VMXOFF
    jne  .not_vmxoff
    jmp  .advance_rip

.not_vmxoff:
    ; ── DR access (0x1D) ─────────────────────────────────────
    cmp  rbx, EXIT_DR
    jne  .not_dr
    jmp  .advance_rip

.not_dr:
    ; ── MONITOR (0x27) ───────────────────────────────────────
    cmp  rbx, EXIT_MONITOR
    jne  .not_monitor
    jmp  .advance_rip

.not_monitor:
    ; ── MWAIT (0x24) ─────────────────────────────────────────
    cmp  rbx, EXIT_MWAIT
    jne  .not_mwait
    jmp  .advance_rip

.not_mwait:
    ; ── VM-entry failure (0x21) ── fatal, halt ────────────────
    cmp  rbx, EXIT_ENTRY_FAIL
    je   .entry_fail_halt

    ; ── Triple fault (0x02) / INIT (0x03) / SIPI (0x04) ──────
    cmp  rbx, EXIT_TRIPL
    je   .advance_rip
    cmp  rbx, EXIT_INIT
    je   near handle_init
    cmp  rbx, EXIT_SIPI
    je   near handle_sipi

    ; ── TRULY Unknown exit: draw EXIT:XX in yellow ───
    ; The VgaDrawChar font has 0-9 at indices 0-9, A-F at 10-15,
    ; X=16, I=17, T=18, :=19. Color is 0x00RRGGBB = yellow=0x00FFFF00.
    mov  eax, [r14 + HDR_FB_CURY]
    cmp  eax, 24
    jb   .print_init_y
    add  eax, 40
    mov  edx, [r14 + HDR_FB_HEIGHT]
    sub  edx, 40
    cmp  eax, edx
    jae  .print_skip
    jmp  .print_save_y
.print_init_y:
    mov  eax, 24
.print_save_y:
    mov  [r14 + HDR_FB_CURY], eax
    mov  dword [r14 + HDR_FB_CURX], 0

    ; 'E' = char index 14
    mov  eax, (0x00FFFF00 | 14)
    call VgaDrawChar
    ; 'X' = char index 16
    mov  eax, (0x00FFFF00 | 16)
    call VgaDrawChar
    ; 'I' = char index 17
    mov  eax, (0x00FFFF00 | 17)
    call VgaDrawChar
    ; 'T' = char index 18
    mov  eax, (0x00FFFF00 | 18)
    call VgaDrawChar
    ; ':' = char index 19
    mov  eax, (0x00FFFF00 | 19)
    call VgaDrawChar
    ; High nibble of exit reason (rbx preserved across VgaDrawChar calls)
    mov  rax, rbx
    shr  rax, 4
    and  rax, 0xF
    or   eax, 0x00FFFF00
    call VgaDrawChar
    ; Low nibble of exit reason
    mov  rax, rbx
    and  rax, 0xF
    or   eax, 0x00FFFF00
    call VgaDrawChar
.print_skip:

    ; ─────────────────────────────────────────────────────────
.advance_rip:
    mov  rcx, 0x440C
    vmread rdx, rcx         ; instruction length
    mov  rcx, 0x681E
    vmread rax, rcx         ; current GUEST_RIP
    add  rax, rdx
    vmwrite rcx, rax        ; write new GUEST_RIP

.resume_only:
    pop  r15
    pop  r14
    pop  r13
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    vmresume
    ; ── VMRESUME FAILED ──────────────────────────────────────
    ; Draw hot-pink failure stripe so we can diagnose this visually.
    call VgaDrawVmresumeFail
    hlt
    jmp  $ - 2

; ── VM-entry failure (0x21): invalid guest state ─────────────
; Fatal: dump all critical VMCS fields to serial + screen, then halt.
.entry_fail_halt:
    ; ── Position Y below existing exit log ──────────────────────
    mov  eax, [r14 + HDR_FB_CURY]
    cmp  eax, 24
    jb   .efh_init_y
    add  eax, 40
.efh_init_y:
    cmp  eax, 24
    jae  .efh_y_set
    mov  eax, 24
.efh_y_set:
    mov  [r14 + HDR_FB_CURY], eax
    mov  dword [r14 + HDR_FB_CURX], 0

    ; ── "EXIT:21" in red ─────────────────────────────────────────
    mov  eax, (0x00FF0000 | 14)  ; E
    call VgaDrawChar
    mov  eax, (0x00FF0000 | 16)  ; X
    call VgaDrawChar
    mov  eax, (0x00FF0000 | 17)  ; I
    call VgaDrawChar
    mov  eax, (0x00FF0000 | 18)  ; T
    call VgaDrawChar
    mov  eax, (0x00FF0000 | 19)  ; :
    call VgaDrawChar
    mov  eax, (0x00FF0000 | 2)   ; 2
    call VgaDrawChar
    mov  eax, (0x00FF0000 | 1)   ; 1
    call VgaDrawChar
    call VgaNewline

    ; ── Serial dump header ────────────────────────────────────────
    lea  rsi, [rel .efh_hdr]
    call AsmPrintStr

    ; ── GUEST_CR0 (0x6800) ───────────────────────────────────────
    lea  rsi, [rel .efh_s_cr0]
    call AsmPrintStr
    mov  rcx, 0x6800
    vmread rax, rcx
    call AsmPrintHex64
    lea  rsi, [rel .efh_crlf]
    call AsmPrintStr
    call VgaDumpHex32
    call VgaNewline

    ; ── GUEST_CR4 (0x6804) ───────────────────────────────────────
    lea  rsi, [rel .efh_s_cr4]
    call AsmPrintStr
    mov  rcx, 0x6804
    vmread rax, rcx
    call AsmPrintHex64
    lea  rsi, [rel .efh_crlf]
    call AsmPrintStr
    call VgaDumpHex32
    call VgaNewline

    ; ── GUEST_EFER (0x2806) ──────────────────────────────────────
    lea  rsi, [rel .efh_s_efr]
    call AsmPrintStr
    mov  rcx, 0x2806
    vmread rax, rcx
    call AsmPrintHex64
    lea  rsi, [rel .efh_crlf]
    call AsmPrintStr
    call VgaDumpHex32
    call VgaNewline

    ; ── VM_ENTRY_CONTROLS (0x4012) ───────────────────────────────
    lea  rsi, [rel .efh_s_enc]
    call AsmPrintStr
    mov  rcx, 0x4012
    vmread rax, rcx
    call AsmPrintHex64
    lea  rsi, [rel .efh_crlf]
    call AsmPrintStr
    call VgaDumpHex32
    call VgaNewline

    ; ── GUEST_CS_AR (0x4816) ─────────────────────────────────────
    lea  rsi, [rel .efh_s_car]
    call AsmPrintStr
    mov  rcx, 0x4816
    vmread rax, rcx
    call AsmPrintHex64
    lea  rsi, [rel .efh_crlf]
    call AsmPrintStr
    call VgaDumpHex32
    call VgaNewline

    ; ── GUEST_TR_SELECTOR (0x080E) ───────────────────────────────
    lea  rsi, [rel .efh_s_trs]
    call AsmPrintStr
    mov  rcx, 0x080E
    vmread rax, rcx
    call AsmPrintHex64
    lea  rsi, [rel .efh_crlf]
    call AsmPrintStr
    call VgaDumpHex32
    call VgaNewline

    ; ── GUEST_TR_AR (0x4822) ─────────────────────────────────────
    lea  rsi, [rel .efh_s_tar]
    call AsmPrintStr
    mov  rcx, 0x4822
    vmread rax, rcx
    call AsmPrintHex64
    lea  rsi, [rel .efh_crlf]
    call AsmPrintStr
    call VgaDumpHex32
    call VgaNewline

    ; ── GUEST_ACTIVITY_STATE (0x4826) ────────────────────────────
    lea  rsi, [rel .efh_s_act]
    call AsmPrintStr
    mov  rcx, 0x4826
    vmread rax, rcx
    call AsmPrintHex64
    lea  rsi, [rel .efh_crlf]
    call AsmPrintStr
    call VgaDumpHex32
    call VgaNewline

    ; ── CPU_BASED_VM_EXEC_CONTROL (0x4002) ──────────────────────────
    lea  rsi, [rel .efh_s_cpu]
    call AsmPrintStr
    mov  rcx, 0x4002
    vmread rax, rcx
    call AsmPrintHex64
    lea  rsi, [rel .efh_crlf]
    call AsmPrintStr
    call VgaDumpHex32
    call VgaNewline

    ; ── SECONDARY_VM_EXEC_CONTROL (0x401E) ──────────────────────────
    lea  rsi, [rel .efh_s_sec]
    call AsmPrintStr
    mov  rcx, 0x401E
    vmread rax, rcx
    call AsmPrintHex64
    lea  rsi, [rel .efh_crlf]
    call AsmPrintStr
    call VgaDumpHex32
    call VgaNewline

    ; ── GUEST_CS_SELECTOR (0x0802) ───────────────────────────────
    lea  rsi, [rel .efh_s_css]
    call AsmPrintStr
    mov  rcx, 0x0802
    vmread rax, rcx
    call AsmPrintHex64
    lea  rsi, [rel .efh_crlf]
    call AsmPrintStr
    call VgaDumpHex32
    call VgaNewline

    ; ── GUEST_SS_AR (0x4818) ─────────────────────────────────────
    lea  rsi, [rel .efh_s_sar]
    call AsmPrintStr
    mov  rcx, 0x4818
    vmread rax, rcx
    call AsmPrintHex64
    lea  rsi, [rel .efh_crlf]
    call AsmPrintStr
    call VgaDumpHex32
    call VgaNewline

    ; ── Draw red diagnostic stripe ────────────────────────────────
    mov  r8, [r14 + HDR_FB_BASE]
    test r8, r8
    jz   .efh_halt
    mov  r9d, dword [r14 + HDR_FB_PPS]
    test r9, r9
    jz   .efh_halt
    xor  rbp, rbp
.efh_stripe_row:
    cmp  rbp, 4
    jge  .efh_halt
    mov  rax, rbp
    add  rax, 8
    imul rax, r9
    shl  rax, 2
    add  rax, r8
    mov  ecx, 320
.efh_stripe_px:
    mov  dword [rax], 0x00FF0000
    add  rax, 4
    dec  ecx
    jnz  .efh_stripe_px
    inc  rbp
    jmp  .efh_stripe_row

.efh_halt:
    hlt
    jmp  $ - 2

.efh_hdr:   db '[FATAL] EXIT:21 VMCS dump:', 13, 10, 0
.efh_s_cr0: db 'CR0=', 0
.efh_s_cr4: db 'CR4=', 0
.efh_s_efr: db 'EFR=', 0
.efh_s_enc: db 'ENC=', 0
.efh_s_car: db 'CAR=', 0
.efh_s_trs: db 'TRS=', 0
.efh_s_tar: db 'TAR=', 0
.efh_s_act: db 'ACT=', 0
.efh_s_cpu: db 'CPU=', 0
.efh_s_sec: db 'SEC=', 0
.efh_s_css: db 'CSS=', 0
.efh_s_sar: db 'SAR=', 0
.efh_crlf:  db 13, 10, 0

; ============================================================
; VgaDumpHex32 - Prints EAX as 8 hex nibbles (yellow) on framebuffer.
; Needs R14 = state header. Preserves all registers.
; ============================================================
VgaDumpHex32:
    push rax
    push rcx
    push r8

    mov  r8d, eax
    mov  ecx, 8
.vdh32_loop:
    rol  r8d, 4
    movzx eax, r8b
    and  eax, 0xF
    or   eax, 0x00FFFF00       ; yellow
    push r8
    push rcx
    call VgaDrawChar
    pop  rcx
    pop  r8
    dec  ecx
    jnz  .vdh32_loop

    pop  r8
    pop  rcx
    pop  rax
    ret

; ============================================================
; VgaNewline - Advance VGA cursor to start of next character line.
; Needs R14 = state header. Preserves all registers.
; ============================================================
VgaNewline:
    push rax
    mov  eax, [r14 + HDR_FB_CURY]
    add  eax, 40
    mov  [r14 + HDR_FB_CURY], eax
    mov  dword [r14 + HDR_FB_CURX], 0
    pop  rax
    ret

AsmPrintHex64:
    push rax
    push rbx
    push rcx
    push rdx
    push r8
    
    mov  r8, rax
    mov  rcx, 16
.loop:
    rol  r8, 4
    mov  rdx, r8
    and  rdx, 0x0F
    add  dl, '0'
    cmp  dl, '9'
    jle  .out_char
    add  dl, 7
.out_char:
    mov  al, dl
    mov  dx, 0x3F8
    out  dx, al
    loop .loop
    
    pop  r8
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    ret

AsmPrintStr:
    push rax
    push rdx
    push rsi
    cld
    mov  dx, 0x3F8
.loop:
    lodsb
    test al, al
    jz   .done
    out  dx, al
    jmp  .loop
.done:
    pop  rsi
    pop  rdx
    pop  rax
    ret

; ============================================================
; VgaDrawMiniPixel - Draws a 4x4 colored block at the top of the screen
; representing the exit reason to act as a real-time visual timeline.
; Input:
;   BL  = Exit Reason low byte
;   R14 = Pointer to State Header
; ============================================================
VgaDrawMiniPixel:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push rbp

    ; Get framebuffer base
    mov r8, [r14 + HDR_FB_BASE]
    test r8, r8
    jz .done

    ; Map exit reason in BL to color in EAX (0x00RRGGBB)
    mov eax, 0x00FFFFFF       ; Default White
    cmp bl, 0x00
    je .c_exc
    cmp bl, 0x01              ; External interrupt
    je .c_extint
    cmp bl, 0x07              ; Interrupt-window
    je .c_intwin
    cmp bl, 0x08              ; NMI-window
    je .c_nmiwin
    cmp bl, 0x0A
    je .c_cpuid
    cmp bl, 0x0C
    je .c_hlt
    cmp bl, 0x0E              ; INVLPG
    je .c_invlpg
    cmp bl, 0x10              ; RDTSC
    je .c_rdtsc
    cmp bl, 0x12
    je .c_vmcall
    cmp bl, 0x1A              ; VMXOFF
    je .c_vmxoff
    cmp bl, 0x1C
    je .c_cr
    cmp bl, 0x1D              ; DR access
    je .c_dr
    cmp bl, 0x1E
    je .c_io
    cmp bl, 0x1F
    je .c_rdmsr
    cmp bl, 0x20
    je .c_wrmsr
    cmp bl, 0x24              ; MWAIT
    je .c_mwait
    cmp bl, 0x27              ; MONITOR
    je .c_monitor
    cmp bl, 0x30
    je .c_ept
    jmp .color_done

.c_exc:
    mov eax, 0x00FF0000       ; Red = Exception/NMI
    jmp .color_done
.c_extint:
    mov eax, 0x0080FF80       ; Light green = External interrupt
    jmp .color_done
.c_intwin:
    mov eax, 0x00FF8080       ; Salmon = Interrupt window
    jmp .color_done
.c_nmiwin:
    mov eax, 0x00FF4040       ; Coral = NMI window
    jmp .color_done
.c_cpuid:
    mov eax, 0x0000FF00       ; Green = CPUID
    jmp .color_done
.c_hlt:
    mov eax, 0x00FFFF00       ; Yellow = HLT
    jmp .color_done
.c_invlpg:
    mov eax, 0x00404040       ; Dark grey = INVLPG
    jmp .color_done
.c_rdtsc:
    mov eax, 0x00806000       ; Dark amber = RDTSC
    jmp .color_done
.c_vmcall:
    mov eax, 0x008080FF       ; Light Blue = VMCALL
    jmp .color_done
.c_vmxoff:
    mov eax, 0x00FF0080       ; Hot magenta = VMXOFF (guest trying to exit VMX)
    jmp .color_done
.c_cr:
    mov eax, 0x000000FF       ; Dark Blue = CR access
    jmp .color_done
.c_dr:
    mov eax, 0x000040C0       ; Medium blue = DR access
    jmp .color_done
.c_io:
    mov eax, 0x00C0C0C0       ; Silver/Grey = IO instruction
    jmp .color_done
.c_rdmsr:
    mov eax, 0x0000FFFF       ; Cyan = RDMSR
    jmp .color_done
.c_wrmsr:
    mov eax, 0x00FF00FF       ; Magenta = WRMSR
    jmp .color_done
.c_mwait:
    mov eax, 0x00804000       ; Dark orange-brown = MWAIT
    jmp .color_done
.c_monitor:
    mov eax, 0x00C06000       ; Amber = MONITOR
    jmp .color_done
.c_ept:
    mov eax, 0x00FF8000       ; Orange = EPT violation
    jmp .color_done

.color_done:
    mov ebx, eax               ; Save mapped color in EBX!

    ; Get and increment exit counter
    mov rcx, [r14 + HDR_EXITCOUNT]

    ; If exit counter is 0, draw static legend
    test rcx, rcx
    jnz .skip_legend
    call VgaDrawStaticLegend
.skip_legend:

    inc qword [r14 + HDR_EXITCOUNT]

    ; Calculate X coordinate: (ExitCount * 4) % Width
    mov  r9d, dword [r14 + HDR_FB_WIDTH]
    test r9, r9
    jz .done

    shl rcx, 2                 ; rcx = ExitCount * 4
    xor rdx, rdx
    mov rax, rcx
    div r9                     ; rdx = (ExitCount * 4) % Width
    mov r10, rdx               ; r10 = X coordinate

    ; Y coordinate is fixed at 4 (second line, Y = 4..7).
    ; Draw 4x4 block in framebuffer.
    mov  r11d, dword [r14 + HDR_FB_PPS] ; Pixels per scanline
    
    xor rbp, rbp               ; rbp = row index (0..3)
.row_loop:
    cmp rbp, 4
    jge .done
    
    ; calculate screen line offset: ((4 + rbp) * PPS + X) * 4
    mov rax, rbp
    add rax, 4                 ; Y = 4 + rbp
    imul rax, r11
    add rax, r10
    shl rax, 2                 ; * 4 bytes per pixel
    add rax, r8                ; add Framebuffer Base

    ; Write 4 horizontal pixels of the color EBX
    mov [rax], ebx
    mov [rax+4], ebx
    mov [rax+8], ebx
    mov [rax+12], ebx

    inc rbp
    jmp .row_loop

.done:
    pop rbp
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; ============================================================
; VgaDrawStaticLegend - Draws a reference color bar at X = 0..39
; representing 10 monitored VM exit reasons in a fixed order.
; ============================================================
VgaDrawStaticLegend:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push rbp

    mov r8, [r14 + HDR_FB_BASE]
    test r8, r8
    jz .legend_done

    mov  r11d, dword [r14 + HDR_FB_PPS] ; Pixels per scanline

    ; Loop through 10 colors
    xor rdi, rdi               ; rdi = color index (0..9)
.color_loop:
    cmp rdi, 10
    jge .legend_done

    ; Get color based on index rdi
    cmp rdi, 0
    jne .not_0
    mov ebx, 0x00FF0000       ; Red = Exception/NMI
    jmp .draw_block
.not_0:
    cmp rdi, 1
    jne .not_1
    mov ebx, 0x0000FF00       ; Green = CPUID
    jmp .draw_block
.not_1:
    cmp rdi, 2
    jne .not_2
    mov ebx, 0x00FFFF00       ; Yellow = HLT
    jmp .draw_block
.not_2:
    cmp rdi, 3
    jne .not_3
    mov ebx, 0x008080FF       ; Light Blue = VMCALL
    jmp .draw_block
.not_3:
    cmp rdi, 4
    jne .not_4
    mov ebx, 0x000000FF       ; Dark Blue = CR access
    jmp .draw_block
.not_4:
    cmp rdi, 5
    jne .not_5
    mov ebx, 0x00C0C0C0       ; Silver/Grey = IO instruction
    jmp .draw_block
.not_5:
    cmp rdi, 6
    jne .not_6
    mov ebx, 0x0000FFFF       ; Cyan = RDMSR
    jmp .draw_block
.not_6:
    cmp rdi, 7
    jne .not_7
    mov ebx, 0x00FF00FF       ; Magenta = WRMSR
    jmp .draw_block
.not_7:
    cmp rdi, 8
    jne .not_8
    mov ebx, 0x00FF8000       ; Orange = EPT violation
    jmp .draw_block
.not_8:
    mov ebx, 0x00FFFFFF       ; White = Default/Other

.draw_block:
    ; X coordinate = rdi * 4
    mov r10, rdi
    shl r10, 2                 ; r10 = X (0, 4, 8, 12, 16, 20, 24, 28, 32, 36)

    ; Draw 4x4 block in framebuffer
    xor rbp, rbp               ; rbp = row index (0..3)
.row_loop:
    cmp rbp, 4
    jge .row_done

    ; calculate screen line offset: (rbp * PPS + X) * 4
    mov rax, rbp
    imul rax, r11
    add rax, r10
    shl rax, 2                 ; * 4 bytes per pixel
    add rax, r8                ; add Framebuffer Base

    ; Write 4 horizontal pixels of the color EBX
    mov [rax], ebx
    mov [rax+4], ebx
    mov [rax+8], ebx
    mov [rax+12], ebx

    inc rbp
    jmp .row_loop

.row_done:
    inc rdi
    jmp .color_loop

.legend_done:
    pop rbp
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; ============================================================
; VgaDrawChar - Draws a scaled 8x8 character on the screen
; Input:
;   AL  = Character index (0..20)
;   R14 = Pointer to State Header
;   EAX (upper 32-bits cleared) = Color (0x00RRGGBB)
; ============================================================
VgaDrawChar:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push rbp

    mov r10d, eax              ; R10D = Color
    movzx r11d, al             ; R11D = Char index

    mov r8, [r14 + HDR_FB_BASE]
    test r8, r8
    jz near .done

    ; Get font data address (RIP-relative)
    lea rsi, [rel .font_data]
    shl r11d, 3                ; index * 8
    add rsi, r11

    ; Get current X and Y
    mov r12d, [r14 + HDR_FB_CURX]
    mov edx, [r14 + HDR_FB_CURY]

    ; Check bounds
    mov eax, [r14 + HDR_FB_WIDTH]
    sub eax, 40
    cmp r12d, eax
    jb short .x_ok
    ; Wrap X, advance Y
    xor r12d, r12d
    add edx, 40
    mov [r14 + HDR_FB_CURY], edx
.x_ok:
    mov eax, [r14 + HDR_FB_HEIGHT]
    sub eax, 40
    cmp edx, eax
    jb short .y_ok
    ; Reset Y
    xor edx, edx
    mov [r14 + HDR_FB_CURY], edx
.y_ok:

    ; We draw 8 font rows
    xor r9d, r9d               ; r9d = row counter (0..7)
.row_loop:
    movzx ebx, byte [rsi + r9] ; EBX = byte pattern of this row

    ; We draw each row of the character as 4 physical screen lines (SCALE = 4)
    mov r11d, 4                ; Scale Y
.scale_y_loop:
    mov edi, ebx               ; Reset pattern for this scale line
    
    ; Calculate starting pixel offset in framebuffer:
    ; Offset = ((Y + row*4 + (4-scale_y)) * PPS + X) * 4
    mov eax, r9d
    shl eax, 2                 ; row * 4
    add eax, edx               ; Y + row * 4
    add eax, 4
    sub eax, r11d              ; Y + row * 4 + (4 - scale_y)

    mov ebp, [r14 + HDR_FB_PPS]
    imul ebp, eax
    add ebp, r12d              ; + X
    shl rbp, 2                 ; * 4
    mov rcx, r8
    add rcx, rbp               ; RCX = pointer to first pixel of this row

    ; Now loop through 8 bits of the byte pattern (from MSB to LSB)
    mov ebp, 8                 ; bit counter
.bit_loop:
    test edi, 0x80
    jz short .bit_zero

    ; Draw a 4-pixel wide block (SCALE = 4)
    mov dword [rcx], r10d
    mov dword [rcx + 4], r10d
    mov dword [rcx + 8], r10d
    mov dword [rcx + 12], r10d
.bit_zero:
    shl edi, 1
    add rcx, 16                ; Move 4 pixels right in framebuffer
    dec ebp
    jnz short .bit_loop

    dec r11d
    jnz near .scale_y_loop

    ; Next row
    inc r9d
    cmp r9d, 8
    jb near .row_loop

    ; Advance X by 40 (32 pixels char + 8 pixels gap)
    add r12d, 40
    mov [r14 + HDR_FB_CURX], r12d

.done:
    pop rbp
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

.font_data:
    db 0x3C, 0x66, 0x6E, 0x7E, 0x76, 0x66, 0x3C, 0x00 ; '0' (0)
    db 0x18, 0x18, 0x38, 0x18, 0x18, 0x18, 0x7E, 0x00 ; '1' (1)
    db 0x3C, 0x66, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00 ; '2' (2)
    db 0x3C, 0x66, 0x0C, 0x3C, 0x0C, 0x66, 0x3C, 0x00 ; '3' (3)
    db 0x06, 0x0E, 0x1E, 0x36, 0x7E, 0x06, 0x06, 0x00 ; '4' (4)
    db 0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00 ; '5' (5)
    db 0x3C, 0x66, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00 ; '6' (6)
    db 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00 ; '7' (7)
    db 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00 ; '8' (8)
    db 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x66, 0x3C, 0x00 ; '9' (9)
    db 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00 ; 'A' (10)
    db 0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00 ; 'B' (11)
    db 0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00 ; 'C' (12)
    db 0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00 ; 'D' (13)
    db 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00 ; 'E' (14)
    db 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00 ; 'F' (15)
    db 0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00 ; 'X' (16)
    db 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 ; 'I' (17)
    db 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 ; 'T' (18)
    db 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00 ; ':' (19)
    db 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 ; ' ' (20)

; ============================================================
; VgaDrawVmresumeFail
; Called immediately after VMRESUME fails (CF=1 from vmresume).
; Draws:
;   Row  8..11  - hot-pink stripe  ("VMRESUME FAILED")
;   Row 12..15  - exit-reason color: R = high_nibble*17, B = low_nibble*17
;   Row 16..19  - VMCS error field color: R = high_nibble*17, B = low_nibble*17
; No parameters; re-finds header via PIC and reads HDR_LAST_EXIT.
; ============================================================
VgaDrawVmresumeFail:
    push rax
    push rbx
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push r14
    push rbp

    ; Re-establish header base via PIC anchor
    lea  rax, [rel .vrf_anchor]
.vrf_anchor:
    sub  rax, (.vrf_anchor - MinimalVmexitHandlerStart) + HDR_SIZE
    mov  r14, rax

    mov  r8, [r14 + HDR_FB_BASE]
    test r8, r8
    jz   .vrf_done

    mov  r9d, dword [r14 + HDR_FB_PPS]
    test r9, r9
    jz   .vrf_done

    ; ── Draw hot-pink stripe at rows 8..11 ──────────────────
    xor  rbp, rbp
.vrf_pink_row:
    cmp  rbp, 4
    jge  .vrf_code_stripe

    mov  rax, rbp
    add  rax, 8
    imul rax, r9
    shl  rax, 2
    add  rax, r8

    mov  ecx, 256
.vrf_pink_px:
    mov  dword [rax], 0x00FF1493   ; hot pink (R=0xFF G=0x14 B=0x93)
    add  rax, 4
    dec  ecx
    jnz  .vrf_pink_px

    inc  rbp
    jmp  .vrf_pink_row

.vrf_code_stripe:
    ; ── Draw exit-reason-encoded color at rows 12..15 ───────
    ; Color: R = high_nibble*17, B = low_nibble*17 -> unique per exit code
    mov  rbx, [r14 + HDR_LAST_EXIT]
    mov  rax, rbx
    shr  rax, 4
    and  rax, 0xF
    imul rax, 17
    mov  rdx, rbx
    and  rdx, 0xF
    imul rdx, 17
    shl  rax, 16               ; → red channel
    or   rax, rdx              ; → blue channel
    mov  r10, rax              ; r10 = exit-reason color

    xor  rbp, rbp
.vrf_code_row:
    cmp  rbp, 4
    jge  .vrf_vmcs_stripe

    mov  rax, rbp
    add  rax, 12
    imul rax, r9
    shl  rax, 2
    add  rax, r8

    mov  ecx, 256
.vrf_code_px:
    mov  [rax], r10d
    add  rax, 4
    dec  ecx
    jnz  .vrf_code_px

    inc  rbp
    jmp  .vrf_code_row

.vrf_vmcs_stripe:
    ; ── Draw VMCS VM_INSTRUCTION_ERROR at rows 16..19 ───────
    ; VMCS field 0x4400 = VM_INSTRUCTION_ERROR (only valid after vmresume fail)
    mov  rcx, 0x4400
    vmread rbx, rcx
    and  rbx, 0xFF
    mov  rax, rbx
    shr  rax, 4
    and  rax, 0xF
    imul rax, 17
    mov  rdx, rbx
    and  rdx, 0xF
    imul rdx, 17
    shl  rax, 8               ; → green channel
    or   rax, rdx             ; → blue channel
    mov  r10, rax             ; r10 = vmcs-error color

    xor  rbp, rbp
.vrf_vmcs_row:
    cmp  rbp, 4
    jge  .vrf_done

    mov  rax, rbp
    add  rax, 16
    imul rax, r9
    shl  rax, 2
    add  rax, r8

    mov  ecx, 256
.vrf_vmcs_px:
    mov  [rax], r10d
    add  rax, 4
    dec  ecx
    jnz  .vrf_vmcs_px

    inc  rbp
    jmp  .vrf_vmcs_row

.vrf_done:
    pop  rbp
    pop  r14
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    ret

handle_init:
    ; ── Handle INIT VM Exit (exit reason 3) ──────────────────
    ; Put the guest vCPU into the Wait-for-SIPI (3) activity state.
    mov  rcx, 0x4826         ; GUEST_ACTIVITY_STATE
    mov  rax, 3
    vmwrite rcx, rax
    pop  r15
    pop  r14
    pop  r13
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    vmresume
    ; VMRESUME FAILED
    call VgaDrawVmresumeFail
    hlt
    jmp  $ - 2

handle_sipi:
    ; ── Handle SIPI VM Exit (exit reason 4) ──────────────────
    ; 1. Read SIPI vector from Exit Qualification (0x6400)
    mov  rcx, 0x6400         ; EXIT_QUALIFICATION
    vmread rdx, rcx          ; rdx contains vector in bits 7:0

    ; 2. Configure Guest CS selector and base according to vector
    ; CS Selector = vector << 8
    mov  rax, rdx
    shl  rax, 8
    mov  rcx, 0x0802         ; GUEST_CS_SELECTOR
    vmwrite rcx, rax

    ; CS Base = vector << 12
    mov  rax, rdx
    shl  rax, 12
    mov  rcx, 0x6808         ; GUEST_CS_BASE
    vmwrite rcx, rax

    ; CS Limit = 0xFFFF
    mov  rax, 0xFFFF
    mov  rcx, 0x4802         ; GUEST_CS_LIMIT
    vmwrite rcx, rax

    ; CS Access Rights = 0x93 (Must be 0x93/0xF3 for real-address mode/unrestricted guest)
    mov  rax, 0x93
    mov  rcx, 0x4816         ; GUEST_CS_AR
    vmwrite rcx, rax

    ; 3. Configure other segment registers (ES, SS, DS, FS, GS)
    ; Selector = 0, Base = 0, Limit = 0xFFFF, AR = 0x93
    xor  rax, rax
    mov  rcx, 0x0800         ; GUEST_ES_SELECTOR
    vmwrite rcx, rax
    mov  rcx, 0x0804         ; GUEST_SS_SELECTOR
    vmwrite rcx, rax
    mov  rcx, 0x0806         ; GUEST_DS_SELECTOR
    vmwrite rcx, rax
    mov  rcx, 0x0808         ; GUEST_FS_SELECTOR
    vmwrite rcx, rax
    mov  rcx, 0x080A         ; GUEST_GS_SELECTOR
    vmwrite rcx, rax

    xor  rax, rax
    mov  rcx, 0x6806         ; GUEST_ES_BASE
    vmwrite rcx, rax
    mov  rcx, 0x680A         ; GUEST_SS_BASE
    vmwrite rcx, rax
    mov  rcx, 0x680C         ; GUEST_DS_BASE
    vmwrite rcx, rax
    mov  rcx, 0x680E         ; GUEST_FS_BASE
    vmwrite rcx, rax
    mov  rcx, 0x6810         ; GUEST_GS_BASE
    vmwrite rcx, rax

    mov  rax, 0xFFFF
    mov  rcx, 0x4800         ; GUEST_ES_LIMIT
    vmwrite rcx, rax
    mov  rcx, 0x4804         ; GUEST_SS_LIMIT
    vmwrite rcx, rax
    mov  rcx, 0x4806         ; GUEST_DS_LIMIT
    vmwrite rcx, rax
    mov  rcx, 0x4808         ; GUEST_FS_LIMIT
    vmwrite rcx, rax
    mov  rcx, 0x480A         ; GUEST_GS_LIMIT
    vmwrite rcx, rax

    mov  rax, 0x93           ; Present, DPL 0, Data, Read/Write, Accessed
    mov  rcx, 0x4814         ; GUEST_ES_AR
    vmwrite rcx, rax
    mov  rcx, 0x4818         ; GUEST_SS_AR
    vmwrite rcx, rax
    mov  rcx, 0x481A         ; GUEST_DS_AR
    vmwrite rcx, rax
    mov  rcx, 0x481C         ; GUEST_FS_AR
    vmwrite rcx, rax
    mov  rcx, 0x481E         ; GUEST_GS_AR
    vmwrite rcx, rax

    ; 4. Configure GDTR and IDTR (Base = 0, Limit = 0xFFFF)
    xor  rax, rax
    mov  rcx, 0x6816         ; GUEST_GDTR_BASE
    vmwrite rcx, rax
    mov  rcx, 0x6818         ; GUEST_IDTR_BASE
    vmwrite rcx, rax

    mov  rax, 0xFFFF
    mov  rcx, 0x4810         ; GUEST_GDTR_LIMIT
    vmwrite rcx, rax
    mov  rcx, 0x4812         ; GUEST_IDTR_LIMIT
    vmwrite rcx, rax

    ; 5. Configure LDTR (Selector = 0, Base = 0, Limit = 0xFFFF, AR = 0x10000 [unusable])
    xor  rax, rax
    mov  rcx, 0x080C         ; GUEST_LDTR_SELECTOR
    vmwrite rcx, rax
    mov  rcx, 0x6812         ; GUEST_LDTR_BASE
    vmwrite rcx, rax
    mov  rax, 0xFFFF
    mov  rcx, 0x480C         ; GUEST_LDTR_LIMIT
    vmwrite rcx, rax
    mov  rax, 0x10000        ; unusable segment
    mov  rcx, 0x4820         ; GUEST_LDTR_AR
    vmwrite rcx, rax
    ; 6. Keep core's GUEST_TR from initialization (must be non-null descriptor)


    ; 7. Configure CR0, CR3, CR4, RFLAGS, RIP, RSP
    ; Desired CR0 = 0x60000010 (CD=1, NW=1, ET=1)
    mov  r8d, 0x60000010

    ; Adjust with FIXED0 (forcing mandatory 1s, except PE and PG)
    mov  ecx, 0x486          ; IA32_VMX_CR0_FIXED0 MSR
    rdmsr                    ; edx:eax = FIXED0
    and  eax, ~0x80000001    ; Clear PE (bit 0) and PG (bit 31)
    or   r8d, eax

    ; Adjust with FIXED1 (forcing mandatory 0s, except PE and PG)
    mov  ecx, 0x487          ; IA32_VMX_CR0_FIXED1 MSR
    rdmsr                    ; edx:eax = FIXED1
    or   eax, 0x80000001    ; Allow PE=1, PG=1
    and  r8d, eax

    ; Write adjusted CR0 to guest state
    mov  rcx, 0x6800         ; GUEST_CR0
    vmwrite rcx, r8

    ; Zero GUEST_INTERRUPTIBILITY (no blocking)
    xor  rax, rax
    mov  rcx, 0x4824         ; GUEST_INTERRUPTIBILITY
    vmwrite rcx, rax

    ; GUEST_CR3 = 0
    xor  rax, rax
    mov  rcx, 0x6802         ; GUEST_CR3
    vmwrite rcx, rax

    ; GUEST_CR4 = dynamically adjusted
    xor  r8d, r8d
    mov  ecx, 0x488          ; IA32_VMX_CR4_FIXED0 MSR
    rdmsr
    ; DO NOT clear VMXE (bit 13) here! Let FIXED0 force it to 1.
    or   r8d, eax
    
    mov  ecx, 0x489          ; IA32_VMX_CR4_FIXED1 MSR
    rdmsr
    and  r8d, eax
    
    mov  rcx, 0x6804         ; GUEST_CR4
    vmwrite rcx, r8

    ; Hide VMXE from guest reads via CR4_READ_SHADOW
    mov  rax, r8
    and  rax, ~0x2000        ; clear VMXE (bit 13) for shadow read
    mov  rcx, 0x6006         ; CR4_READ_SHADOW
    vmwrite rcx, rax

    ; GUEST_RFLAGS = 2
    mov  rax, 2
    mov  rcx, 0x6820         ; GUEST_RFLAGS
    vmwrite rcx, rax

    ; GUEST_RIP = 0
    xor  rax, rax
    mov  rcx, 0x681E         ; GUEST_RIP
    vmwrite rcx, rax

    ; GUEST_RSP = 0
    xor  rax, rax
    mov  rcx, 0x681C         ; GUEST_RSP
    vmwrite rcx, rax

    ; GUEST_ACTIVITY_STATE = 0 (Active)
    xor  rax, rax
    mov  rcx, 0x4826         ; GUEST_ACTIVITY_STATE
    vmwrite rcx, rax

    ; 8. Clear IA-32e guest mode
    mov  rcx, 0x4012
    vmread r8, rcx               ; r8 = current VM_ENTRY_CONTROLS

    ; Jump over debug strings
    jmp  .sipi_dbg_start
.sipi_dbg_prefix: db '[SIPI] ENC old: ', 0
.sipi_dbg_mid:    db ' new: ', 0
.sipi_crlf:       db 13, 10, 0
.sipi_dbg_start:

    ; Print old ENC to serial port
    push rcx
    push rdx
    push rax
    lea  rsi, [rel .sipi_dbg_prefix]
    call AsmPrintStr
    mov  rax, r8
    call AsmPrintHex64
    pop  rax
    pop  rdx
    pop  rcx

    ; Read IA32_VMX_TRUE_ENTRY_CTLS (0x490) since bit 55 of IA32_VMX_BASIC is 1
    mov  ecx, 0x490
    rdmsr                        ; eax=FIXED0, edx=FIXED1

    and  eax, ~(1 << 9)          ; remove IA-32e from mandatory set
    or   r8d, eax                ; apply remaining mandatory-1 bits
    and  r8d, edx                ; mask out bits FIXED1 says must be 0

    ; Explicitly clear bit 9 last — overrides everything
    btr  r8, 9

    mov  rcx, 0x4012
    vmwrite rcx, r8

    ; Read back and print new ENC
    vmread r9, rcx
    push rcx
    push rdx
    push rax
    lea  rsi, [rel .sipi_dbg_mid]
    call AsmPrintStr
    mov  rax, r9
    call AsmPrintHex64
    lea  rsi, [rel .sipi_crlf]
    call AsmPrintStr
    pop  rax
    pop  rdx
    pop  rcx

    ; 8.1. Explicitly configure VMCS Link Pointer and DR7 for strict real-mode compliance
    mov  rcx, 0x2800         ; GUEST_VMCS_LINK_PTR
    mov  rax, 0xFFFFFFFFFFFFFFFF
    vmwrite rcx, rax

    mov  rcx, 0x681A         ; GUEST_DR7
    mov  rax, 0x400
    vmwrite rcx, rax

    ; Zero GUEST_EFER so the guest doesn't run with LME=1
    xor  rax, rax
    mov  rcx, 0x2806         ; GUEST_EFER
    vmwrite rcx, rax

    ; 8.5. Clear 64-bit addresses to 32-bit (0) for non-IA-32e mode compliance!
    ; In non-IA-32e mode, SYSENTER_ESP, SYSENTER_EIP, and TR_BASE *must* have bits 63:32 clear.
    ; Since Windows was running in 64-bit mode, these are full 64-bit kernel addresses.
    mov  rcx, 0x6824         ; GUEST_SYSENTER_ESP
    vmwrite rcx, rax
    mov  rcx, 0x6826         ; GUEST_SYSENTER_EIP
    vmwrite rcx, rax
    mov  rcx, 0x6814         ; GUEST_TR_BASE
    vmwrite rcx, rax

    ; 9. Zero all guest General Purpose Registers
    xor  rsi, rsi
    xor  rdi, rdi
    xor  rbp, rbp
    xor  r8, r8
    xor  r9, r9
    xor  r10, r10
    xor  r11, r11
    xor  r12, r12
    mov  qword [rsp + STK_RAX], 0
    mov  qword [rsp + STK_RBX], 0
    mov  qword [rsp + STK_RCX], 0
    mov  qword [rsp + STK_RDX], 0
    mov  qword [rsp + STK_R13], 0
    mov  qword [rsp + STK_R14], 0
    mov  qword [rsp + STK_R15], 0

    pop  r15
    pop  r14
    pop  r13
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    vmresume
    ; VMRESUME FAILED
    call VgaDrawVmresumeFail
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

    ; Set RAX=1 so the guest resumes with a non-zero "return value" (success indicator)
    mov  rax, 1

    ; Launch
    vmlaunch

    ; If we get here, VMLAUNCH failed - overwrite rax with 0
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
