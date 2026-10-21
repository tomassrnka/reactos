/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS system libraries
 * FILE:        dll/win32/kernel32/client/amd64/fiber.S
 * PURPOSE:     Fiber context switch code for the amd64 architecture
 * PROGRAMMER:
 */

#include <asm.inc>
#include <ksamd64.inc>

EXTERN BaseThreadStartup:PROC

.code64

/*
 * First code that runs on a new fiber. SwitchToFiber "returns" here, because
 * BaseInitializeContext put this address on the new stack. RSP is 16 byte
 * aligned at this point and we are on the fiber's own stack.
 */
PUBLIC BaseFiberStartup
FUNC BaseFiberStartup
    /* Shadow space for the call; keeps RSP 16 byte aligned at the call */
    sub rsp, 32
    .ALLOCSTACK 32
    .ENDPROLOG

    /* Get the fiber data */
    mov rax, gs:[TeFiberData]

    /* Start the thread with our parameters (RCX = routine, RDX = parameter) */
    mov rcx, [rax + FbFiberContext + CxRcx]
    mov rdx, [rax + FbFiberContext + CxRdx]
    call BaseThreadStartup

    /* BaseThreadStartup never returns */
    int 3
ENDFUNC


/*
 * VOID WINAPI SwitchToFiber(_In_ PVOID Fiber);
 *
 * RCX = new fiber. Saves the non-volatile state of the current fiber into
 * its FIBER structure and resumes the new one. The XMM6-XMM15 and MXCSR
 * state is always switched (it is callee saved in the x64 ABI), which is
 * the documented behavior on x64, whatever FIBER_FLAG_FLOAT_SWITCH says.
 */
PUBLIC SwitchToFiber
FUNC SwitchToFiber
    .ENDPROLOG

    /* Get the TEB and the current fiber */
    mov rdx, gs:[TeSelf]
    mov rax, [rdx + TeFiberData]

    /* Save the non-volatile registers; RSP points to our return address */
    mov [rax + FbFiberContext + CxRbx], rbx
    mov [rax + FbFiberContext + CxRbp], rbp
    mov [rax + FbFiberContext + CxRsi], rsi
    mov [rax + FbFiberContext + CxRdi], rdi
    mov [rax + FbFiberContext + CxR12], r12
    mov [rax + FbFiberContext + CxR13], r13
    mov [rax + FbFiberContext + CxR14], r14
    mov [rax + FbFiberContext + CxR15], r15
    mov [rax + FbFiberContext + CxRsp], rsp

    /* Save the SIMD state */
    stmxcsr [rax + FbFiberContext + CxMxCsr]
    movups [rax + FbFiberContext + CxXmm6], xmm6
    movups [rax + FbFiberContext + CxXmm7], xmm7
    movups [rax + FbFiberContext + CxXmm8], xmm8
    movups [rax + FbFiberContext + CxXmm9], xmm9
    movups [rax + FbFiberContext + CxXmm10], xmm10
    movups [rax + FbFiberContext + CxXmm11], xmm11
    movups [rax + FbFiberContext + CxXmm12], xmm12
    movups [rax + FbFiberContext + CxXmm13], xmm13
    movups [rax + FbFiberContext + CxXmm14], xmm14
    movups [rax + FbFiberContext + CxXmm15], xmm15

    /* Transfer some data from the TEB */
    mov r8, [rdx + TeFlsData]
    mov [rax + FbFlsData], r8
    mov r8, [rdx + TeActivationContextStackPointer]
    mov [rax + FbActivationContextStackPointer], r8

    /* Transfer some data related to the Stack */
    mov r8, [rdx + TeStackLimit]
    mov [rax + FbStackLimit], r8
    mov r8d, [rdx + TeGuaranteedStackBytes]
    mov [rax + FbGuaranteedStackBytes], r8d

    /* Switch to the new fiber */
    mov [rdx + TeFiberData], rcx

    /* Switch Fiber Data */
    mov r8, [rcx + FbStackBase]
    mov [rdx + TeStackBase], r8
    mov r8, [rcx + FbStackLimit]
    mov [rdx + TeStackLimit], r8
    mov r8, [rcx + FbDeallocationStack]
    mov [rdx + TeDeallocationStack], r8
    mov r8d, [rcx + FbGuaranteedStackBytes]
    mov [rdx + TeGuaranteedStackBytes], r8d
    mov r8, [rcx + FbActivationContextStackPointer]
    mov [rdx + TeActivationContextStackPointer], r8
    mov r8, [rcx + FbFlsData]
    mov [rdx + TeFlsData], r8

    /* Restore the SIMD state */
    ldmxcsr [rcx + FbFiberContext + CxMxCsr]
    movups xmm6, [rcx + FbFiberContext + CxXmm6]
    movups xmm7, [rcx + FbFiberContext + CxXmm7]
    movups xmm8, [rcx + FbFiberContext + CxXmm8]
    movups xmm9, [rcx + FbFiberContext + CxXmm9]
    movups xmm10, [rcx + FbFiberContext + CxXmm10]
    movups xmm11, [rcx + FbFiberContext + CxXmm11]
    movups xmm12, [rcx + FbFiberContext + CxXmm12]
    movups xmm13, [rcx + FbFiberContext + CxXmm13]
    movups xmm14, [rcx + FbFiberContext + CxXmm14]
    movups xmm15, [rcx + FbFiberContext + CxXmm15]

    /* Restore the non-volatile registers */
    mov rbx, [rcx + FbFiberContext + CxRbx]
    mov rbp, [rcx + FbFiberContext + CxRbp]
    mov rsi, [rcx + FbFiberContext + CxRsi]
    mov rdi, [rcx + FbFiberContext + CxRdi]
    mov r12, [rcx + FbFiberContext + CxR12]
    mov r13, [rcx + FbFiberContext + CxR13]
    mov r14, [rcx + FbFiberContext + CxR14]
    mov r15, [rcx + FbFiberContext + CxR15]

    /* Jump to the new fiber: return to where it called SwitchToFiber from
       (or into BaseFiberStartup for a fiber that was never run) */
    mov rsp, [rcx + FbFiberContext + CxRsp]
    ret
ENDFUNC

END
